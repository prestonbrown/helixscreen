// SPDX-License-Identifier: GPL-3.0-or-later
// Every number the feedback loop decides on. Nothing else in the loop hard-codes one.

/** Distinct devices a crash cluster needs before it raises an alert. */
export const CLUSTER_ALERT_DEVICES = 3
/** Distinct devices a crash cluster needs before it files an issue. */
export const CLUSTER_ISSUE_DEVICES = 3
/** Distinct devices an error code needs before it files an issue. */
export const ERROR_ISSUE_DEVICES = 5
/** Distinct devices failing the same update step before an issue is filed. */
export const UPDATE_FAILURE_ISSUE_DEVICES = 3
/** Devices a release needs before its crash-free rate is compared to the previous one. */
export const RELEASE_REGRESSION_MIN_DEVICES = 20
/** Crash-free rate drop (as a fraction) that marks a release as a regression. */
export const RELEASE_REGRESSION_CRASH_FREE_DROP = 0.02
/** Devices reporting an unknown printer before a printer-DB candidate issue is filed. */
export const PRINTER_ISSUE_DEVICES = 5
/** New issues a single run may file; the rest wait for the next run. */
export const NEW_ISSUES_PER_RUN = 5
/** Days before the same alert may be emailed again. */
export const ALERT_REPEAT_DAYS = 30
/** A main_loop_hang with less host memory than this (MB available) is the host running out, not the app blocking. */
export const HANG_HOST_MEMORY_LOW_MB = 64
/**
 * Seconds of stall past which the whole app was paused, not its main loop. The detector samples
 * every 5 s and reports as a stall crosses 60 s, so a report of a longer stall than this means
 * the monitor thread itself stopped running.
 */
export const HANG_SUSPENDED_STALL_SEC = 70
/**
 * Seconds of device uptime before a hang that a memory snapshot may sit and still describe
 * the host at the hang; snapshots are taken hourly.
 */
export const HANG_SNAPSHOT_WINDOW_SEC = 3600
/**
 * Seconds between the Analytics Engine write times of events from one upload. A device
 * uploads about once a day, so a hang and that day's snapshots share a write time, and
 * only the uptime each carries says which snapshot came first.
 */
export const HANG_UPLOAD_BATCH_SEC = 10
/**
 * Seconds of device uptime before a hang within which a memory warning describes the host at
 * the hang. A client reports pressure as its level rises or its RSS grows, not on a timer, so
 * a warning older than this may describe a state the host has left.
 */
export const HANG_WARNING_WINDOW_SEC = 900
/** The share of a week's hangs that must be classifiable before the week is compared to the next. */
export const HANG_MIN_CLASSIFIED_SHARE = 0.5
