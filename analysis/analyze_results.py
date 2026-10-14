#!/usr/bin/env python3
"""Analysis of the static-vs-dynamic 5G slicing experiments.

Reads   results/time_series.csv, results/static_results.csv, results/dynamic_results.csv
Writes  plots/*.png and results/summary_statistics.csv
Compatible with Python >= 3.6 (Ubuntu 18.04).

NOTE: FlowMonitor's rxBytes counts the IP packet (payload + 20 B IP + 8 B UDP header), while
'offered' counts application payload only. Throughput is therefore converted back to payload
throughput here: payload / (payload + 28), per slice (see apply_header_correction).
"""
import argparse
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
import pandas as pd  # noqa: E402

SLICES = ["eMBB", "URLLC", "mMTC"]
MODES = ["static", "dynamic"]
COLORS = {"eMBB": "tab:blue", "URLLC": "tab:red", "mMTC": "tab:green"}
MODE_COLORS = {"static": "tab:gray", "dynamic": "tab:orange"}

TS_COLS = ["mode", "seed", "time", "slice", "offered_mbps", "throughput_mbps", "latency_ms",
           "packet_loss_percent", "jitter_ms", "resource_percent", "utilization_percent",
           "qos_satisfied"]
SUM_COLS = ["mode", "seed", "slice", "avg_throughput_mbps", "avg_offered_mbps", "avg_latency_ms",
            "packet_loss_percent", "jitter_ms", "tx_packets", "rx_packets", "offered_packets",
            "policed_packets", "avg_resource_percent", "avg_utilization_percent",
            "qos_satisfaction_ratio"]

HEADER_BYTES = 28  # IPv4 (20) + UDP (8)


def fail(msg):
    print("VALIDATION ERROR: " + msg)
    sys.exit(1)


def load(path, cols):
    if not os.path.isfile(path):
        fail("missing file " + path)
    df = pd.read_csv(path)
    missing = [c for c in cols if c not in df.columns]
    if missing:
        fail("%s lacks columns %s" % (path, missing))
    if df.empty:
        fail(path + " is empty")
    return df


def apply_header_correction(ts, st, dy, pkt):
    """Convert FlowMonitor IP-level throughput to application-payload throughput."""
    factor = {sl: pkt[sl] / float(pkt[sl] + HEADER_BYTES) for sl in SLICES}
    print("Header correction (payload / (payload + %d B)): %s"
          % (HEADER_BYTES, ", ".join("%s x%.4f" % (s, factor[s]) for s in SLICES)))
    ts = ts.copy()
    st = st.copy()
    dy = dy.copy()
    f_ts = ts["slice"].map(factor)
    ts["throughput_mbps"] = ts["throughput_mbps"] * f_ts
    ts["utilization_percent"] = ts["utilization_percent"] * f_ts
    for df in (st, dy):
        f = df["slice"].map(factor)
        df["avg_throughput_mbps"] = df["avg_throughput_mbps"] * f
        df["avg_utilization_percent"] = df["avg_utilization_percent"] * f
    return ts, st, dy


def validate(ts, st, dy):
    for m in MODES:
        sub = ts[ts["mode"] == m]
        if sub.empty:
            fail("time_series.csv has no rows for mode '%s'" % m)
        if set(sub["slice"].unique()) != set(SLICES):
            fail("mode %s: slices present %s" % (m, sorted(sub["slice"].unique())))

    dup = ts.duplicated(subset=["mode", "seed", "time", "slice"]).sum()
    if dup > 0:
        fail("%d duplicate (mode, seed, time, slice) rows. Delete results/*.csv and re-run "
             "each experiment once." % dup)

    for col in ["throughput_mbps", "offered_mbps", "packet_loss_percent", "resource_percent"]:
        if (ts[col] < -1e-9).any():
            fail("negative values in " + col)
    if (ts["packet_loss_percent"] > 100 + 1e-9).any():
        fail("packet loss > 100 %")

    sums = ts.groupby(["mode", "seed", "time"])["resource_percent"].sum()
    if not np.allclose(sums.values, 100.0, atol=0.01):
        fail("resource shares do not sum to 100 %% (min %.3f, max %.3f)"
             % (sums.min(), sums.max()))
    if not ts["qos_satisfied"].isin([0, 1]).all():
        fail("qos_satisfied must be 0/1")

    # Over a whole run a slice cannot receive more payload than was offered
    # (windows are 1 s, so summing Mbps over windows gives Mbit).
    tot = ts.groupby(["mode", "seed", "slice"])[["throughput_mbps", "offered_mbps"]].sum()
    bad_runs = tot[tot["throughput_mbps"] > 1.02 * tot["offered_mbps"] + 0.05]
    if len(bad_runs) > 0:
        print(bad_runs)
        fail("received more than offered over a full run even after header correction "
             "(check --pkt-embb/--pkt-urllc/--pkt-mmtc match the simulation)")

    over = ts[ts["throughput_mbps"] > 1.25 * ts["offered_mbps"] + 0.05]
    if len(over) > 0:
        print("NOTE: %d of %d windows have throughput above the offered load "
              "(queued data draining after overload; expected, not an error)."
              % (len(over), len(ts)))

    for df, name in [(st, "static_results.csv"), (dy, "dynamic_results.csv")]:
        if (df["rx_packets"] > df["offered_packets"] + 1).any():
            fail(name + ": rx_packets > offered_packets")
    print("Validation passed: %d time-series rows, seeds: %s"
          % (len(ts), sorted(ts["seed"].unique())))


def phase_index(ts, phase_len):
    ts = ts.copy()
    ts["phase"] = (ts["time"] // phase_len).astype(int)
    return ts


def save(fig, plots, name):
    path = os.path.join(plots, name)
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print("saved " + path)


def phase_lines(ax, tmax, phase_len):
    t = phase_len
    while t < tmax:
        ax.axvline(t, color="k", ls=":", lw=0.7)
        t += phase_len


def time_plot(ts, column, ylabel, title, fname, plots, phase_len, logy=False):
    fig, axes = plt.subplots(1, 2, figsize=(13, 4.5), sharey=True)
    tmax = ts["time"].max() + 1
    for ax, mode in zip(axes, MODES):
        for sl in SLICES:
            d = ts[(ts["mode"] == mode) & (ts["slice"] == sl)]
            g = d.groupby("time")[column].agg(["mean", "std"]).reset_index()
            ax.plot(g["time"], g["mean"], label=sl, color=COLORS[sl], marker="o", ms=2.5)
            if d["seed"].nunique() > 1:
                ax.fill_between(g["time"], g["mean"] - g["std"].fillna(0),
                                g["mean"] + g["std"].fillna(0), color=COLORS[sl], alpha=0.15)
        phase_lines(ax, tmax, phase_len)
        ax.set_title("%s - %s slicing" % (title, mode))
        ax.set_xlabel("Time (s) [start of 1 s measurement window]")
        ax.grid(alpha=0.3)
        if logy:
            ax.set_yscale("log")
        ax.legend()
    axes[0].set_ylabel(ylabel)
    save(fig, plots, fname)


def resource_plot(ts, plots, phase_len):
    fig, axes = plt.subplots(1, 2, figsize=(13, 4.5), sharey=True)
    tmax = ts["time"].max() + 1
    for ax, mode in zip(axes, MODES):
        d = ts[ts["mode"] == mode]
        for sl in SLICES:
            g = d[d["slice"] == sl].groupby("time")["resource_percent"].mean()
            ax.step(g.index, g.values, where="post", label=sl, color=COLORS[sl])
        phase_lines(ax, tmax, phase_len)
        ax.set_ylim(0, 100)
        ax.set_title("Resource allocation - %s slicing" % mode)
        ax.set_xlabel("Time (s)")
        ax.grid(alpha=0.3)
        ax.legend()
    axes[0].set_ylabel("Allocated share of cell capacity (%)")
    save(fig, plots, "plot4_resource_allocation_vs_time.png")


def comparison_plot(ts, column, ylabel, title, fname, plots, phase_len):
    ts = phase_index(ts, phase_len)
    phases = sorted(ts["phase"].unique())
    fig, axes = plt.subplots(1, 3, figsize=(16, 4.5))
    w = 0.38
    x = np.arange(len(phases))
    for ax, sl in zip(axes, SLICES):
        for k, mode in enumerate(MODES):
            d = ts[(ts["mode"] == mode) & (ts["slice"] == sl)]
            per_seed = d.groupby(["seed", "phase"])[column].mean().unstack("seed")
            per_seed = per_seed.reindex(phases)
            m = per_seed.mean(axis=1).values
            s = per_seed.std(axis=1).fillna(0).values
            ax.bar(x + (k - 0.5) * w, np.nan_to_num(m), w, yerr=s, capsize=2,
                   label=mode, color=MODE_COLORS[mode])
        ax.set_xticks(x)
        ax.set_xticklabels(["%d-%d s" % (p * phase_len, (p + 1) * phase_len) for p in phases],
                           rotation=20)
        ax.set_title("%s: %s" % (sl, title))
        ax.set_xlabel("Traffic phase")
        ax.set_ylabel(ylabel)
        ax.grid(axis="y", alpha=0.3)
        ax.legend()
    save(fig, plots, fname)


def overall_qos_plot(ts, plots):
    fig, ax = plt.subplots(figsize=(7, 4.5))
    names = SLICES + ["All slices"]
    x = np.arange(len(names))
    w = 0.38
    for k, mode in enumerate(MODES):
        d = ts[ts["mode"] == mode]
        vals, errs = [], []
        for sl in SLICES:
            per_seed = d[d["slice"] == sl].groupby("seed")["qos_satisfied"].mean()
            vals.append(per_seed.mean())
            errs.append(per_seed.std() if len(per_seed) > 1 else 0.0)
        per_seed = d.groupby("seed")["qos_satisfied"].mean()
        vals.append(per_seed.mean())
        errs.append(per_seed.std() if len(per_seed) > 1 else 0.0)
        ax.bar(x + (k - 0.5) * w, vals, w, yerr=np.nan_to_num(errs), capsize=3, label=mode,
               color=MODE_COLORS[mode])
    ax.set_xticks(x)
    ax.set_xticklabels(names)
    ax.set_ylim(0, 1.05)
    ax.set_ylabel("QoS satisfaction ratio (fraction of 1 s windows)")
    ax.set_title("QoS satisfaction: static vs dynamic slicing (whole run)")
    ax.grid(axis="y", alpha=0.3)
    ax.legend()
    save(fig, plots, "plot8b_qos_satisfaction_overall.png")


def summary_table(st, dy, results):
    both = pd.concat([st, dy], ignore_index=True)
    metrics = ["avg_throughput_mbps", "avg_latency_ms", "packet_loss_percent", "jitter_ms",
               "avg_resource_percent", "avg_utilization_percent", "qos_satisfaction_ratio"]
    g = both.groupby(["mode", "slice"])[metrics].agg(["mean", "std"])
    g.columns = ["%s_%s" % (a, b) for a, b in g.columns]
    g = g.reset_index()
    out = os.path.join(results, "summary_statistics.csv")
    g.to_csv(out, index=False)
    print("saved " + out)
    pd.set_option("display.width", 200)
    pd.set_option("display.max_columns", 30)
    print("\n=== Mean over seeds (per run totals) ===")
    print(both.groupby(["slice", "mode"])[metrics].mean().round(3))
    print("\n=== Dynamic minus static "
          "(positive throughput/qos = better; negative latency/loss = better) ===")
    m = both.groupby(["slice", "mode"])[metrics].mean()
    for sl in SLICES:
        try:
            diff = m.loc[(sl, "dynamic")] - m.loc[(sl, "static")]
            print(sl + ": " + ", ".join("%s=%+.3f" % (k, v) for k, v in diff.items()))
        except KeyError:
            pass


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default=os.path.join(root, "results"))
    ap.add_argument("--plots", default=os.path.join(root, "plots"))
    ap.add_argument("--phase-length", type=float, default=10.0, help="traffic phase length (s)")
    ap.add_argument("--pkt-embb", type=int, default=1400, help="eMBB payload bytes (as simulated)")
    ap.add_argument("--pkt-urllc", type=int, default=100, help="URLLC payload bytes")
    ap.add_argument("--pkt-mmtc", type=int, default=100, help="mMTC payload bytes")
    ap.add_argument("--no-header-correction", action="store_true",
                    help="do not convert IP-level throughput to payload throughput")
    a = ap.parse_args()
    os.makedirs(a.plots, exist_ok=True)

    ts = load(os.path.join(a.results, "time_series.csv"), TS_COLS)
    st = load(os.path.join(a.results, "static_results.csv"), SUM_COLS)
    dy = load(os.path.join(a.results, "dynamic_results.csv"), SUM_COLS)
    if not a.no_header_correction:
        pkt = {"eMBB": a.pkt_embb, "URLLC": a.pkt_urllc, "mMTC": a.pkt_mmtc}
        ts, st, dy = apply_header_correction(ts, st, dy, pkt)
    validate(ts, st, dy)

    pl = a.phase_length
    time_plot(ts, "throughput_mbps", "Throughput (Mbps, application payload)", "Throughput",
              "plot1_throughput_vs_time.png", a.plots, pl)
    time_plot(ts, "latency_ms", "End-to-end latency (ms)", "Latency",
              "plot2_latency_vs_time.png", a.plots, pl, logy=True)
    time_plot(ts, "packet_loss_percent", "Packet loss (%)", "Packet loss",
              "plot3_packet_loss_vs_time.png", a.plots, pl)
    resource_plot(ts, a.plots, pl)
    comparison_plot(ts, "throughput_mbps", "Mean throughput (Mbps, payload)", "throughput",
                    "plot5_static_vs_dynamic_throughput.png", a.plots, pl)
    comparison_plot(ts, "latency_ms", "Mean latency (ms)", "latency",
                    "plot6_static_vs_dynamic_latency.png", a.plots, pl)
    comparison_plot(ts, "packet_loss_percent", "Mean packet loss (%)", "packet loss",
                    "plot7_static_vs_dynamic_packet_loss.png", a.plots, pl)
    comparison_plot(ts, "qos_satisfied", "QoS satisfaction ratio", "QoS satisfaction",
                    "plot8_qos_satisfaction_per_phase.png", a.plots, pl)
    overall_qos_plot(ts, a.plots)
    summary_table(st, dy, a.results)


if __name__ == "__main__":
    main()
