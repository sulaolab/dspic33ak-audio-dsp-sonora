"""Console-line compatibility shim for the TDM load metrics (report S26).

The engine line changed shape when the load metric was re-based on the union profiler:

  old  TDMsum:max=244.2us(73.2%)margin=88.8us sat=0 win=TDM1/333.3us bound=75.1%
  new  TDMsum:load=105.3% max=350.5us over=5.3us sat=7 win=TDM1/333.3us

and the per-leg line renamed `max=` to `resp=`, because that figure is a RESPONSE time
(preemption included), not a load.

Every measurement tool in this tree -- and every archived .jsonl of captured console text --
was written against the old shape. Rather than edit a dozen group-numbered regexes twice
(once for the new shape, once to keep parsing history), each tool normalises its line source
through to_legacy() and keeps its existing patterns.

The two derived fields are honest, not invented:
  * `margin=-X` from `over=X` -- the overrun IS a negative margin, the same quantity.
  * `bound=` is re-emitted as load. That is exactly why the field was dropped: once the
    denominator is the union, the collision bound and the measured load are the same number,
    so a tool that prints `bound` keeps printing something true.

A value above 100% now reaches the tools instead of being clamped. Consumers that treated
100.0% as "saturated" still see sat=, which is unchanged.
"""

import re

_LEG = re.compile(r"(TDM\d:)resp=")
_SUM = re.compile(r"(?P<pre>.*TDMsum:)load=(?P<pct>[\d.]+)% max=(?P<max>[\d.]+)us "
                  r"(?P<kind>over|margin)=(?P<val>[\d.]+)us sat=(?P<sat>\d+)(?P<rest>.*)$")


def to_legacy(line):
    """Rewrite one console line into the pre-S26 shape. Old lines pass through unchanged."""
    line = _LEG.sub(r"\1max=", line)
    m = _SUM.match(line)
    if m is None:
        return line
    g = m.groupdict()
    margin = ("-" if g["kind"] == "over" else "") + g["val"]
    eol = line[len(line.rstrip("\r\n")):]
    rest = g["rest"].rstrip("\r\n")
    if "bound=" not in rest:
        rest += " bound=%s%%" % g["pct"]
    return "%smax=%sus(%s%%)margin=%sus sat=%s%s%s" % (
        g["pre"], g["max"], g["pct"], margin, g["sat"], rest, eol)


def to_legacy_all(lines):
    return [to_legacy(l) for l in lines]
