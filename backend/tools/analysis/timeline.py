"""Sampling-timeline checks for captured CSVs.

Pure by design — a sequence in, data out. Standard library only, so this is
testable without pandas, matplotlib, scipy or pybaselines, none of which the
backend venv carries and none of which the server needs.
"""

import statistics

# Loose on purpose. The seam this exists to catch is an idle timeout between two
# acquisitions in one file — minutes, against a 20 ms sampling interval — so a
# threshold anywhere near the sampling rate would report jitter instead.
DEFAULT_GAP_FACTOR = 10


def find_time_gaps(timestamps, factor=DEFAULT_GAP_FACTOR):
    """Find discontinuities in a sequence of millisecond timestamps.

    A gap is a step that is either backwards — a uint32 millis wrap or a device
    reset — or more than `factor` times the median forward step. The median is
    used rather than a fixed interval on purpose: the analysis tool's DT is an
    ASSUMPTION about the capture, and a check that assumed the same thing could
    not detect the assumption being wrong.

    Args:
        timestamps: sequence of ints, in the order they were recorded
        factor:     how many median steps count as a discontinuity

    Returns:
        [(index, delta), ...] where index is the position of the sample that
        arrives AFTER the discontinuity. Empty when there is nothing to judge:
        fewer than two samples, or no forward step to take a median from.

    A capture whose timestamps are all identical has a median step of zero, so a
    median-scaled check has no opinion and this returns nothing. That is a
    different defect — a capture with no working clock — and it needs its own
    check rather than this one reporting every row.
    """
    if len(timestamps) < 2:
        return []

    deltas = [timestamps[i] - timestamps[i - 1] for i in range(1, len(timestamps))]

    # The median comes from forward steps only: one backwards jump of four
    # billion would otherwise drag the reference it is being measured against.
    forward = [d for d in deltas if d > 0]
    if not forward:
        return []

    threshold = statistics.median(forward) * factor

    return [(i + 1, d) for i, d in enumerate(deltas) if d <= 0 or d > threshold]
