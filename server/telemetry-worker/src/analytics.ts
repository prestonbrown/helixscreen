// SPDX-License-Identifier: GPL-3.0-or-later
// Maps telemetry events to Analytics Engine data points for real-time querying.
// Which columns a point has, and where, is declared in schema.ts.

import {
  EVENT_FANOUT,
  MAX_SLOT,
  POINT_KINDS,
  SAVER_MAPS,
  type EventRecord,
  type FieldType,
} from "./schema";

export interface AnalyticsEngineDataPoint {
  blobs?: string[];
  doubles?: number[];
  indexes?: string[];
}

// Arrays are padded to at least the shape every point has always had.
const MIN_BLOBS = 12;
const MIN_DOUBLES = 8;

/**
 * Maps a raw telemetry event to one or more Analytics Engine data points.
 * Most events produce a single data point; panel_usage is expanded into
 * one data point per panel (since C++ sends an aggregate per-session event).
 */
export function mapEventToDataPoints(event: EventRecord): AnalyticsEngineDataPoint[] {
  const eventType = String(event.event ?? "unknown");
  // An event type is client-supplied, so inherited names ("constructor") are unknown too.
  if (!Object.prototype.hasOwnProperty.call(EVENT_FANOUT, eventType)) {
    return [unknownEventPoint(eventType, event)];
  }
  const kinds = EVENT_FANOUT[eventType];

  const points: AnalyticsEngineDataPoint[] = [];
  for (const kind of kinds) {
    for (const item of fanOutItems(kind, event)) {
      points.push(buildPoint(kind, event, item));
    }
  }
  return points;
}

function slotCount(kind: string, type: FieldType, minimum: number): number {
  let highest = minimum;
  for (const c of POINT_KINDS[kind].columns) {
    if (c.type === type && c.slot > highest) highest = c.slot;
  }
  return Math.min(highest, MAX_SLOT);
}

/** Fills one point's blobs and doubles by slot; a missing value is "" or 0. */
function buildPoint(kind: string, event: EventRecord, item?: EventRecord): AnalyticsEngineDataPoint {
  const blobs: string[] = new Array(slotCount(kind, "blob", MIN_BLOBS)).fill("");
  const doubles: number[] = new Array(slotCount(kind, "double", MIN_DOUBLES)).fill(0);

  for (const c of POINT_KINDS[kind].columns) {
    const raw = c.read ? c.read(event, item) : event[c.field];
    if (c.type === "blob") blobs[c.slot - 1] = String(raw ?? "");
    else doubles[c.slot - 1] = Number(raw ?? 0);
  }
  return { indexes: [kind], blobs, doubles };
}

/** One point of `kind` from `record`, for the points the Worker writes itself. */
export function pointOfKind(kind: string, record: EventRecord): AnalyticsEngineDataPoint {
  return buildPoint(kind, record);
}

// ---------- Fan-out ----------

const NO_ITEM: Array<EventRecord | undefined> = [undefined];

function countedItems(
  map: unknown,
  nameKey: string,
): Array<EventRecord | undefined> {
  return Object.entries((map ?? {}) as Record<string, unknown>).map(([name, count]) => ({
    [nameKey]: name,
    count,
  }));
}

/** The panels named by either of panel_usage's per-panel maps. */
function panelNames(event: EventRecord): Set<string> {
  const timeMap = (event.panel_time_sec ?? {}) as Record<string, unknown>;
  const visitMap = (event.panel_visits ?? {}) as Record<string, unknown>;
  return new Set([...Object.keys(timeMap), ...Object.keys(visitMap)]);
}

/**
 * The elements a point kind iterates for one event: one point per element.
 * A kind with no iteration gets a single item-less point; an empty list means
 * the kind writes nothing for this event.
 */
function fanOutItems(kind: string, event: EventRecord): Array<EventRecord | undefined> {
  switch (kind) {
    case "panel_usage": {
      const names = panelNames(event);
      if (names.size === 0) {
        // Flat format: the event is already one panel.
        return [{ panel_name: event.panel_name, time_sec: event.time_sec, visits: event.visits }];
      }
      const timeMap = (event.panel_time_sec ?? {}) as Record<string, unknown>;
      const visitMap = (event.panel_visits ?? {}) as Record<string, unknown>;
      return [...names].map((panel) => ({
        panel_name: panel,
        time_sec: timeMap[panel],
        visits: visitMap[panel],
      }));
    }

    // Only the expanded per-panel format carries these maps.
    case "widget_interaction":
      return panelNames(event).size > 0
        ? countedItems(event.widget_interactions, "widget_id")
        : [];

    case "overlay_visit":
      return panelNames(event).size > 0
        ? countedItems(event.overlay_visits, "overlay_name")
        : [];

    case "screensaver_usage": {
      const map = (field: string) => (event[field] ?? {}) as Record<string, unknown>;
      const names = new Set(SAVER_MAPS.flatMap((m) => Object.keys(map(m))));
      return [...names].map((saver_name) => ({
        saver_name,
        shown_sec: map("saver_shown_sec")[saver_name],
        starts: map("saver_starts")[saver_name],
        woke_by_touch: map("saver_woke_by_touch")[saver_name],
        stopped_other: map("saver_stopped_other")[saver_name],
        too_heavy: map("saver_too_heavy")[saver_name],
      }));
    }

    // Keys are "<saver>:<level>"; a saver name never contains ':'.
    case "screensaver_level":
      return Object.entries((event.saver_settled_levels ?? {}) as Record<string, unknown>).map(
        ([key, runs]) => {
          const at = key.lastIndexOf(":");
          const level = at < 0 ? NaN : Number(key.slice(at + 1));
          return {
            saver_name: at < 0 ? key : key.slice(0, at),
            level: Number.isFinite(level) ? level : undefined,
            runs,
          };
        },
      );

    case "widget_placement": {
      const homeWidgets = event.home_widgets;
      if (!Array.isArray(homeWidgets)) return [];
      const counts = new Map<string, number>();
      for (const w of homeWidgets) {
        const id = String(w);
        counts.set(id, (counts.get(id) ?? 0) + 1);
      }
      return [...counts].map(([widget_id, count]) => ({ widget_id, count }));
    }

    case "settings_change": {
      const changes = (event.changes ?? []) as EventRecord[];
      return changes.length > 0 ? changes.map((c) => c ?? {}) : NO_ITEM;
    }

    default:
      return NO_ITEM;
  }
}

/** Unknown event types still write basic info so they stay discoverable. */
function unknownEventPoint(eventType: string, event: EventRecord): AnalyticsEngineDataPoint {
  const blobs = new Array<string>(MIN_BLOBS).fill("");
  blobs[0] = String(event.device_id ?? "");
  blobs[1] = eventType;
  return { indexes: [eventType], blobs, doubles: new Array<number>(MIN_DOUBLES).fill(0) };
}
