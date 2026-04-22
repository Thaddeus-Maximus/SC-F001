"""Battery voltage calibration math.

Firmware model (power_mgmt.c:278):
    V_bat = mV * V_SENS_K + V_SENS_OFFSET
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass
class CalResult:
    k: float
    offset: float
    note: str


def single_point_offset(bat_mv: float, v_true: float, k: float) -> CalResult:
    """Fix K, solve for OFFSET so that V_bat(bat_mv) == v_true."""
    offset = v_true - bat_mv * k
    return CalResult(k=k, offset=offset, note="single-point offset")


def two_point(
    bat_mv_1: float, v_true_1: float,
    bat_mv_2: float, v_true_2: float,
) -> CalResult:
    """Solve for K and OFFSET from two (mV, V) pairs."""
    dmv = bat_mv_2 - bat_mv_1
    if abs(dmv) < 1e-3:
        raise ValueError("Two calibration points are too close — pick wider V")
    k = (v_true_2 - v_true_1) / dmv
    offset = v_true_1 - bat_mv_1 * k
    return CalResult(k=k, offset=offset, note="two-point")


def verify(bat_mv: float, cal: CalResult) -> float:
    return bat_mv * cal.k + cal.offset
