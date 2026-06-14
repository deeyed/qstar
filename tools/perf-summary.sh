#!/bin/sh
set -eu

format=${QSTAR_PERF_SUMMARY_FORMAT:-text}
repeat=
ratio_x100=${QSTAR_PERF_RATIO_X100:-200}
slack_ms=${QSTAR_PERF_RATIO_SLACK_MS:-250}
hard=0
label=${QSTAR_PERF_SUMMARY_LABEL:-}

usage() {
	cat <<'EOF'
usage: tools/perf-summary.sh [options] [file...]
       tools/perf-summary.sh [options] --repeat N -- command [arg...]

Options:
  --format text|markdown   Output line protocol or release-note markdown.
  --repeat N               Run the command N times and summarize combined output.
  --ratio-x100 N           Ratio threshold for non-ninja backends. Default: 200.
  --slack-ms N             Absolute slack added to ratio threshold. Default: 250.
  --hard                   Exit non-zero when a ratio threshold is exceeded.
  --label NAME             Heading used by markdown output.
  -h, --help               Show this help.
EOF
}

fail() {
	echo "qstar-perf-summary: $*" >&2
	exit 1
}

is_uint() {
	case "$1" in
	''|*[!0-9]*)
		return 1
		;;
	esac
	return 0
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--format)
		shift
		[ "$#" -gt 0 ] || fail "--format requires a value"
		format=$1
		;;
	--format=*)
		format=${1#--format=}
		;;
	--repeat)
		shift
		[ "$#" -gt 0 ] || fail "--repeat requires a value"
		repeat=$1
		;;
	--repeat=*)
		repeat=${1#--repeat=}
		;;
	--ratio-x100)
		shift
		[ "$#" -gt 0 ] || fail "--ratio-x100 requires a value"
		ratio_x100=$1
		;;
	--ratio-x100=*)
		ratio_x100=${1#--ratio-x100=}
		;;
	--slack-ms)
		shift
		[ "$#" -gt 0 ] || fail "--slack-ms requires a value"
		slack_ms=$1
		;;
	--slack-ms=*)
		slack_ms=${1#--slack-ms=}
		;;
	--hard)
		hard=1
		;;
	--label)
		shift
		[ "$#" -gt 0 ] || fail "--label requires a value"
		label=$1
		;;
	--label=*)
		label=${1#--label=}
		;;
	-h|--help)
		usage
		exit 0
		;;
	--)
		shift
		break
		;;
	-*)
		fail "unknown option '$1'"
		;;
	*)
		break
		;;
	esac
	shift
done

case "$format" in
text|markdown)
	;;
*)
	fail "invalid --format '$format'"
	;;
esac
is_uint "$ratio_x100" || fail "--ratio-x100 must be an unsigned integer"
is_uint "$slack_ms" || fail "--slack-ms must be an unsigned integer"

tmp=
if [ -n "$repeat" ]; then
	is_uint "$repeat" || fail "--repeat must be an unsigned integer"
	[ "$repeat" -gt 0 ] || fail "--repeat must be greater than zero"
	[ "$#" -gt 0 ] || fail "--repeat requires a command after --"
	tmp=${TMPDIR:-/tmp}/qstar-perf-summary.$$
	raw=$tmp/raw.out
	mkdir -p "$tmp"
	trap 'rm -rf "$tmp"' EXIT HUP INT TERM
	i=1
	while [ "$i" -le "$repeat" ]; do
		echo "qstar-perf-summary: run $i/$repeat" >&2
		if ! "$@" >> "$raw" 2>> "$tmp/raw.err"; then
			cat "$raw" >&2 2>/dev/null || true
			cat "$tmp/raw.err" >&2 2>/dev/null || true
			fail "command failed during repeat $i"
		fi
		i=$((i + 1))
	done
	set -- "$raw"
fi

awk \
	-v format="$format" \
	-v ratio_limit="$ratio_x100" \
	-v slack="$slack_ms" \
	-v hard="$hard" \
	-v label="$label" '
function field_value(name,    i, prefix) {
	prefix = name "="
	for (i = 2; i <= NF; i++) {
		if (index($i, prefix) == 1) {
			return substr($i, length(prefix) + 1)
		}
	}
	return ""
}

function remember_sample(gate, mode, backend, phase, elapsed,    key) {
	key = gate "|" mode "|" backend "|" phase
	if (!(key in seen)) {
		seen[key] = 1
		keys[++key_count] = key
	}
	values[key, ++counts[key]] = elapsed + 0
}

function sort_values(key, n,    i, j, t) {
	for (i = 1; i <= n; i++) {
		sorted[i] = values[key, i]
	}
	for (i = 2; i <= n; i++) {
		t = sorted[i]
		j = i - 1
		while (j >= 1 && sorted[j] > t) {
			sorted[j + 1] = sorted[j]
			j--
		}
		sorted[j + 1] = t
	}
}

function rounded(v) {
	return int(v + 0.5)
}

function emit_sample_text(gate, mode, backend, phase, n, min, median, max) {
	printf "perf_summary sample gate=%s mode=%s backend=%s phase=%s count=%d min_ms=%d median_ms=%d max_ms=%d\n",
		gate, mode, backend, phase, n, min, median, max
}

function emit_sample_markdown(gate, mode, backend, phase, n, min, median, max) {
	printf "| %s | %s | %s | %s | %d | %d | %d | %d |\n",
		gate, mode, backend, phase, n, min, median, max
}

function emit_ratio_text(gate, mode, backend, phase, backend_median, ninja_median, ratio, status) {
	printf "perf_summary ratio gate=%s mode=%s backend=%s phase=%s backend_median_ms=%d ninja_median_ms=%d ratio_x100=%d threshold_x100=%d slack_ms=%d status=%s\n",
		gate, mode, backend, phase, backend_median, ninja_median, ratio, ratio_limit, slack, status
}

function emit_ratio_markdown(gate, mode, backend, phase, backend_median, ninja_median, ratio, status) {
	printf "| %s | %s | %s | %s | %d | %d | %.2fx | %s |\n",
		gate, mode, backend, phase, backend_median, ninja_median, ratio / 100.0, status
}

function split_key(key, out) {
	split(key, out, "|")
}

$1 == "medium_project_gate" || $1 == "large_project_gate" {
	gate = ($1 == "medium_project_gate") ? "medium" : "large"
	mode = field_value("mode")
	if (mode == "") {
		mode = (gate == "medium") ? "medium" : "default"
	}
	backend = field_value("backend")
	phase = field_value("phase")
	elapsed = field_value("elapsed_ms")
	if (backend != "" && phase != "" && elapsed ~ /^[0-9]+$/) {
		remember_sample(gate, mode, backend, phase, elapsed)
	}
}

END {
	if (format == "markdown") {
		if (label != "") {
			printf "## %s\n\n", label
		}
		print "| Gate | Mode | Backend | Phase | Count | Min ms | Median ms | Max ms |"
		print "| --- | --- | --- | --- | ---: | ---: | ---: | ---: |"
	}

	for (i = 1; i <= key_count; i++) {
		key = keys[i]
		n = counts[key]
		sort_values(key, n)
		min = sorted[1]
		max = sorted[n]
		if (n % 2 == 1) {
			median = sorted[(n + 1) / 2]
		} else {
			median = rounded((sorted[n / 2] + sorted[n / 2 + 1]) / 2)
		}
		medians[key] = median
		split_key(key, parts)
		if (format == "markdown") {
			emit_sample_markdown(parts[1], parts[2], parts[3], parts[4], n, min, median, max)
		} else {
			emit_sample_text(parts[1], parts[2], parts[3], parts[4], n, min, median, max)
		}
		delete sorted
	}

	if (format == "markdown") {
		print ""
		print "| Gate | Mode | Backend | Phase | Backend median ms | Ninja median ms | Ratio | Status |"
		print "| --- | --- | --- | --- | ---: | ---: | ---: | --- |"
	}

	for (i = 1; i <= key_count; i++) {
		key = keys[i]
		split_key(key, parts)
		gate = parts[1]
		mode = parts[2]
		backend = parts[3]
		phase = parts[4]
		if (backend == "ninja") {
			continue
		}
		ninja_key = gate "|" mode "|ninja|" phase
		if (!(ninja_key in medians) || medians[ninja_key] <= 0) {
			continue
		}
		backend_median = medians[key]
		ninja_median = medians[ninja_key]
		ratio = rounded((backend_median * 100) / ninja_median)
		limit_ms = (ninja_median * ratio_limit) / 100.0 + slack
		status = (backend_median <= limit_ms) ? "ok" : "warn"
		ratio_count++
		if (status == "warn") {
			warning_count++
		}
		if (format == "markdown") {
			emit_ratio_markdown(gate, mode, backend, phase, backend_median, ninja_median, ratio, status)
		} else {
			emit_ratio_text(gate, mode, backend, phase, backend_median, ninja_median, ratio, status)
		}
	}

	if (format == "text") {
		status = (warning_count == 0) ? "ok" : "warn"
		printf "perf_summary status=%s sample_count=%d ratio_count=%d warning_count=%d hard=%d threshold_x100=%d slack_ms=%d\n",
			status, key_count, ratio_count, warning_count + 0, hard, ratio_limit, slack
	}
	if (hard && warning_count > 0) {
		exit 2
	}
}
' "$@"
