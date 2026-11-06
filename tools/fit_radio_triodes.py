"""Offline characteristic fit; requires NumPy. No receiver load-line targets.

Input readings are approximate graphical transcriptions, not precise tube data.
RCA's 1941 type-75 sheet explicitly refers to the 6SQ7 characteristics/curves.
The tabulated operating points are withheld from the fit as separate checks.
Run from any directory; prints parameters only, never modifies production code.
"""
import csv
from pathlib import Path

import numpy as np


def current(plate, grid, mu, parameters):
    kg, kp, kvb = np.exp(parameters)
    activation = kp * (1 / mu + grid / np.sqrt(kvb + plate * plate))
    e = plate / kp * np.logaddexp(0, activation)
    return np.power(e, 1.5) / kg * 1000


def fit(rows, mu):
    plate, grid, observed, uncertainty = np.array([
        [float(r[k]) for k in ("plate_v", "grid_v", "current_ma", "uncertainty_ma")]
        for r in rows if r["role"] == "fit"]).T

    def residual(p):
        return (current(plate, grid, mu, p) - observed) / uncertainty

    best = (float("inf"), None)
    for kp in (50, 150, 500, 1500):
        for kvb in (10, 100, 1000, 10000):
            p = np.log([1000, kp, kvb])
            damping = .01
            for _ in range(150):
                r = residual(p)
                j = np.column_stack([(residual(p + np.eye(3)[i] * 1e-5) - r) / 1e-5
                                     for i in range(3)])
                delta = np.linalg.solve(j.T @ j + damping * np.eye(3), -j.T @ r)
                trial = np.clip(p + np.clip(delta, -.8, .8), np.log(.01), np.log(1e7))
                if np.linalg.norm(residual(trial)) < np.linalg.norm(r):
                    p = trial
                    damping = max(damping / 3, 1e-8)
                else:
                    damping *= 5
                if np.linalg.norm(delta) < 1e-9:
                    break
            error = np.linalg.norm(residual(p))
            if error < best[0]:
                best = (error, p)
    return best


if __name__ == "__main__":
    path = Path(__file__).resolve().parents[1] / "docs/data/radio1938-triode-characteristics.csv"
    with path.open() as f:
        readings = list(csv.DictReader(f))
    for tube, mu in (("6J5", 20), ("75", 100)):
        rows = [r for r in readings if r["tube"] == tube]
        error, p = fit(rows, mu)
        print(tube, "mu/ex/kg1/kp/kvb", mu, 1.5, *np.exp(p), "weighted norm", error)
        for r in rows:
            if r["role"] == "check":
                value = current(float(r["plate_v"]), float(r["grid_v"]), mu, p)
                print("  withheld", r["plate_v"], r["grid_v"], "expected", r["current_ma"], "mA; got", value)
        for bias in ([-4., -3.7, -3.5] if tube == "6J5" else [-.5, -.6, -.7]):
            supply, resistance = (250, 150000) if tube == "6J5" else (200, 190000)
            low, high = 0., float(supply)
            for _ in range(60):
                v = (low + high) / 2
                if current(v, bias, mu, p) * .001 > (supply - v) / resistance:
                    high = v
                else:
                    low = v
            print("  loadline bias", bias, "plate", v, "current mA", (supply-v)/resistance*1000)
