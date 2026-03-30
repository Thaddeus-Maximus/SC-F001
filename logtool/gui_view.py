"""
Matplotlib GUI for SC-F001 logtool.
"""

from parser import LOG_TYPE_BAT, LOG_TYPE_CRASH, _ts_to_str

try:
    import matplotlib.pyplot as plt
    import matplotlib.dates as mdates
    from matplotlib.animation import FuncAnimation
    import numpy as np
    _MPL_OK = True
except ImportError:
    _MPL_OK = False


def _check_mpl():
    if not _MPL_OK:
        raise ImportError("'matplotlib' and 'numpy' required for GUI mode. Install: pip install matplotlib")


def _entries_to_arrays(entries: list) -> dict:
    """Split entries into typed arrays for plotting."""
    fsm   = [e for e in entries if 0 <= e.get('entry_type', -1) <= 12]
    bat   = [e for e in entries if e.get('entry_type') == LOG_TYPE_BAT]
    crash = [e for e in entries if e.get('entry_type') == LOG_TYPE_CRASH]
    return {'fsm': fsm, 'bat': bat, 'crash': crash}


def _ts_arr(entries, key='ts_ms'):
    import numpy as np
    return np.array([e.get(key, 0) / 1000.0 for e in entries])


def _val_arr(entries, key):
    import numpy as np
    return np.array([e.get(key, float('nan')) for e in entries])


def show_plots(entries: list, title: str = "SC-F001 Log"):
    _check_mpl()
    import matplotlib.pyplot as plt
    import matplotlib.dates as mdates
    import numpy as np
    from datetime import datetime

    arrays = _entries_to_arrays(entries)
    fsm = arrays['fsm']
    bat = arrays['bat']
    crash = arrays['crash']
    crash_ts = [e.get('ts_ms', 0) / 1000.0 for e in crash]

    fig, axes = plt.subplots(4, 1, figsize=(14, 10), sharex=True)
    fig.suptitle(title)

    def add_crash_lines(ax):
        for ts in crash_ts:
            ax.axvline(x=datetime.utcfromtimestamp(ts), color='red',
                       linestyle='--', linewidth=1.0, alpha=0.7)

    def to_dt(ts_arr):
        return [datetime.utcfromtimestamp(t) for t in ts_arr]

    # 1. Battery voltage
    ax0 = axes[0]
    ax0.set_ylabel('Battery (V)')
    all_bat_entries = fsm + bat
    all_bat_entries.sort(key=lambda e: e.get('ts_ms', 0))
    if all_bat_entries:
        ts = to_dt(_ts_arr(all_bat_entries))
        vs = _val_arr(all_bat_entries, 'bat_V')
        ax0.plot(ts, vs, color='green', linewidth=1)
    add_crash_lines(ax0)
    ax0.grid(True, alpha=0.3)

    # 2. Currents
    ax1 = axes[1]
    ax1.set_ylabel('Current (A)')
    if fsm:
        ts = to_dt(_ts_arr(fsm))
        ax1.plot(ts, _val_arr(fsm, 'drive_A'), label='Drive', linewidth=1)
        ax1.plot(ts, _val_arr(fsm, 'jack_A'),  label='Jack',  linewidth=1)
        ax1.plot(ts, _val_arr(fsm, 'aux_A'),   label='Aux',   linewidth=1)
        ax1.legend(fontsize=8, loc='upper right')
    add_crash_lines(ax1)
    ax1.grid(True, alpha=0.3)

    # 3. FSM state
    ax2 = axes[2]
    ax2.set_ylabel('FSM State')
    if fsm:
        ts = to_dt(_ts_arr(fsm))
        states = _val_arr(fsm, 'entry_type')
        ax2.step(ts, states, where='post', linewidth=1, color='navy')
        # y-tick labels: use state names from first entry if available
        state_map = {}
        for e in fsm:
            state_map[e['entry_type']] = e.get('state_name', str(e['entry_type']))
        yticks = sorted(state_map.keys())
        ax2.set_yticks(yticks)
        ax2.set_yticklabels([state_map[k] for k in yticks], fontsize=7)
    add_crash_lines(ax2)
    ax2.grid(True, alpha=0.3)

    # 4. Thermal accumulators
    ax3 = axes[3]
    ax3.set_ylabel('Heat (I²t)')
    if fsm:
        ts = to_dt(_ts_arr(fsm))
        ax3.plot(ts, _val_arr(fsm, 'drive_heat'), label='Drive', linewidth=1)
        ax3.plot(ts, _val_arr(fsm, 'jack_heat'),  label='Jack',  linewidth=1)
        ax3.plot(ts, _val_arr(fsm, 'aux_heat'),   label='Aux',   linewidth=1)
        ax3.legend(fontsize=8, loc='upper right')
    add_crash_lines(ax3)
    ax3.grid(True, alpha=0.3)

    ax3.xaxis.set_major_formatter(mdates.AutoDateFormatter(ax3.xaxis.get_major_locator()))
    fig.autofmt_xdate()
    plt.tight_layout()
    plt.show()


def live_plot(url: str, interval_s: float = 2.0):
    """Live-updating matplotlib plot using FuncAnimation."""
    _check_mpl()
    import matplotlib.pyplot as plt
    import matplotlib.dates as mdates
    from matplotlib.animation import FuncAnimation
    from datetime import datetime
    import source as src
    import parser as prs

    all_entries = []

    fig, axes = plt.subplots(4, 1, figsize=(14, 10), sharex=True)
    fig.suptitle("SC-F001 Live Log")
    labels = ['Battery (V)', 'Current (A)', 'FSM State', 'Heat (I²t)']
    for ax, lbl in zip(axes, labels):
        ax.set_ylabel(lbl)
        ax.grid(True, alpha=0.3)

    lines = {
        'bat':   axes[0].plot([], [], color='green', linewidth=1)[0],
        'drive': axes[1].plot([], [], label='Drive', linewidth=1)[0],
        'jack':  axes[1].plot([], [], label='Jack',  linewidth=1)[0],
        'aux':   axes[1].plot([], [], label='Aux',   linewidth=1)[0],
        'state': axes[2].step([], [], where='post', linewidth=1, color='navy')[0],
        'drheat': axes[3].plot([], [], label='Drive', linewidth=1)[0],
        'jkheat': axes[3].plot([], [], label='Jack',  linewidth=1)[0],
        'axheat': axes[3].plot([], [], label='Aux',   linewidth=1)[0],
    }
    axes[1].legend(fontsize=8, loc='upper right')
    axes[3].legend(fontsize=8, loc='upper right')
    axes[3].xaxis.set_major_formatter(mdates.AutoDateFormatter(axes[3].xaxis.get_major_locator()))

    state = {'current_tail': 0, 'first': True}

    def to_dt(ts_list):
        return [datetime.utcfromtimestamp(t / 1000.0) for t in ts_list]

    def update(_frame):
        try:
            if state['first']:
                blob = src.fetch_full(url)
                meta, tail, head, new_entries = prs.autodetect_and_parse(blob)
                state['current_tail'] = head or 0
                state['first'] = False
            else:
                binary, new_head = src.fetch_incremental(url, state['current_tail'])
                new_entries = prs.parse_entries(binary) if binary else []
                state['current_tail'] = new_head

            all_entries.extend(new_entries)
        except Exception as exc:
            print(f"[live_plot] fetch error: {exc}")
            return

        fsm   = [e for e in all_entries if 0 <= e.get('entry_type', -1) <= 12]
        bat   = [e for e in all_entries if e.get('entry_type') in (LOG_TYPE_BAT,) or 'bat_V' in e]
        crash = [e for e in all_entries if e.get('entry_type') == LOG_TYPE_CRASH]

        if fsm:
            ts = to_dt([e['ts_ms'] for e in fsm])
            lines['drive'].set_data(ts, [e.get('drive_A', 0) for e in fsm])
            lines['jack'].set_data( ts, [e.get('jack_A', 0)  for e in fsm])
            lines['aux'].set_data(  ts, [e.get('aux_A', 0)   for e in fsm])
            lines['state'].set_data(ts, [e.get('entry_type', 0) for e in fsm])
            lines['drheat'].set_data(ts, [e.get('drive_heat', 0) for e in fsm])
            lines['jkheat'].set_data(ts, [e.get('jack_heat', 0)  for e in fsm])
            lines['axheat'].set_data(ts, [e.get('aux_heat', 0)   for e in fsm])

        all_bat = sorted(
            [e for e in all_entries if 'bat_V' in e],
            key=lambda e: e.get('ts_ms', 0)
        )
        if all_bat:
            ts = to_dt([e['ts_ms'] for e in all_bat])
            lines['bat'].set_data(ts, [e['bat_V'] for e in all_bat])

        for ax in axes:
            ax.relim()
            ax.autoscale_view()
        fig.autofmt_xdate()

    ani = FuncAnimation(fig, update, interval=interval_s * 1000, cache_frame_data=False)
    plt.tight_layout()
    plt.show()
