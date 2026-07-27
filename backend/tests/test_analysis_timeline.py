"""Tests for the sampling-timeline check used by the CSV analysis tool.

The analysis script integrates acceleration on a hardcoded uniform grid
(DT = 0.02 s) and has never read the timestamp column. A CSV can hold two
acquisitions separated by an idle timeout — up to 20 minutes — because the
device keeps its ring across one, so nothing marks the seam and the hole is
integrated as if it were 20 ms.

This module is pure by design: a sequence in, data out. It imports nothing
beyond the standard library, so the check is testable without pandas,
matplotlib, scipy or pybaselines — none of which the backend venv has, and none
of which the server needs.
"""

import pytest

from tools.analysis.timeline import find_time_gaps


def test_no_gaps_in_uniform_timestamps():
    assert find_time_gaps([0, 20, 40, 60, 80]) == []


def test_ignores_ordinary_jitter():
    """Real captures are not metronomes. 19/21/22 ms is sampling, not a seam."""
    assert find_time_gaps([0, 19, 40, 62, 80, 99]) == []


def test_detects_a_single_large_gap():
    """The S1 case: an idle timeout between two acquisitions in one file."""
    timestamps = [0, 20, 40, 1200040, 1200060]

    gaps = find_time_gaps(timestamps)

    assert len(gaps) == 1
    index, delta = gaps[0]
    assert index == 3          # the sample that arrives after the hole
    assert delta == 1200000


def test_detects_a_counter_reset():
    """A uint32 millis wrap or a device reset shows up as a negative delta. It
    is a discontinuity in the timeline, not something to skip."""
    timestamps = [4294967000, 4294967020, 15, 35]

    gaps = find_time_gaps(timestamps)

    assert len(gaps) == 1
    assert gaps[0][0] == 2


def test_handles_empty_and_single_row_inputs():
    """Fewer than two samples cannot have a delta, let alone a gap."""
    assert find_time_gaps([])    == []
    assert find_time_gaps([100]) == []


def test_a_degenerate_timeline_is_not_judged():
    """Every timestamp identical means the median delta is zero, and a check
    scaled to the median has no opinion. That is a different defect — a capture
    with no working clock — and it needs a different check, not this one
    reporting every row as a gap.
    """
    assert find_time_gaps([7, 7, 7, 7]) == []


def test_factor_is_adjustable():
    """The default is deliberately loose. A caller looking for smaller seams
    can tighten it without the function assuming a sampling rate."""
    timestamps = [0, 20, 40, 140, 160]

    assert find_time_gaps(timestamps)             == []
    assert len(find_time_gaps(timestamps, factor=3)) == 1
