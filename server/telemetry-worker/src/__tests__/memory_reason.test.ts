// SPDX-License-Identifier: GPL-3.0-or-later
import { describe, expect, it } from "vitest";
import { mapEventToDataPoints } from "../analytics";
import { memoryReasonCode } from "../schema";

describe("memoryReasonCode", () => {
  it("prefers the code the client sends", () => {
    expect(memoryReasonCode("rss_warning", "anything 12MB")).toBe("rss_warning");
  });

  it("groups the same wording at different measurements", () => {
    const a = memoryReasonCode(undefined, "RSS 30MB exceeds warning threshold 30MB");
    const b = memoryReasonCode(undefined, "RSS 31MB exceeds warning threshold 30MB");
    expect(a).toBe("rss_warning");
    expect(b).toBe(a);
  });

  it.each([
    ["RSS 200MB exceeds critical threshold 180MB", "rss_critical"],
    ["System available 9MB below critical threshold 16MB", "available_critical"],
    ["System available 20MB below warning threshold 32MB", "available_warning"],
    ["System available 20MB held below warning threshold 32MB", "sustained_low_available"],
    ["RSS growth +3072KB over 5 minutes exceeds threshold 3072KB", "growth"],
    ["Pressure level warning held by hysteresis (RSS 5MB, available 9MB)", "hysteresis"],
  ])("maps %s", (text, code) => {
    expect(memoryReasonCode(undefined, text)).toBe(code);
  });

  it("slugs unknown text with the numbers dropped", () => {
    expect(memoryReasonCode(undefined, "Swap use 12MB high")).toBe(memoryReasonCode(undefined, "Swap use 99MB high"));
    expect(memoryReasonCode(undefined, "Swap use 12MB high")).toBe("swap_use_mb_high");
  });

  it("is empty when the event has neither field", () => {
    expect(memoryReasonCode(undefined, undefined)).toBe("");
  });

  it("is what memory_warning stores in blob5", () => {
    const [point] = mapEventToDataPoints({
      event: "memory_warning",
      device_id: "d",
      reason: "RSS 31MB exceeds warning threshold 30MB",
    });
    expect(point.blobs?.[4]).toBe("rss_warning");
  });
});
