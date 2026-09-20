#!/usr/bin/env bash
# run_all_tests.sh — единый test runner для всех модулей KeySidekick
# Usage: ./run_all_tests.sh
# Exit code: 0 если все прошли, 1 если хотя бы один упал
set -e

cd "$(dirname "$0")"
GXX="${GXX:-C:/MinGW64/bin/g++.exe}"
# Предупреждения включены всегда — те же классы, что в build.bat. Раньше наборы
# собирались вообще без флагов, из-за чего, например, съеденная комментарием
# строка таблицы scan-кодов и падение на power-resume дошли до релиза.
CXXFLAGS="-std=c++14 -D_WIN32_WINNT=0x0600 -Wall -Wextra -Wno-missing-field-initializers"
OBJDIR=".ai-cache"
mkdir -p "$OBJDIR"
# Чистим прошлые логи сборки: в .ai-cache остаются файлы от прежних экспериментов
# (например command_queue_modern.*), и устаревший warning в них сделал бы гейт
# ниже вечно красным без видимой причины.
rm -f "$OBJDIR"/*.build.log

passed=0
failed=0
failed_names=()

run_test() {
    local name="$1"
    local sources="$2"
    local libs="$3"
    local extra_flags="$4"

    # Build
    if ! $GXX $CXXFLAGS $extra_flags -o "$OBJDIR/${name}.exe" $sources $libs 2>"$OBJDIR/${name}.build.log"; then
        echo "  BUILD FAIL: $name (see $OBJDIR/${name}.build.log)"
        failed=$((failed+1))
        failed_names+=("$name")
        return
    fi

    # Run. On failure, surface the test's own output: the suites print the
    # failing expression, and throwing it away left red builds with no cause.
    if "$OBJDIR/${name}.exe" >"$OBJDIR/${name}.run.log" 2>&1; then
        echo "  ✓ $name"
        passed=$((passed+1))
    else
        echo "  ✗ $name (runtime)"
        sed 's/^/      /' "$OBJDIR/${name}.run.log" | tail -20
        failed=$((failed+1))
        failed_names+=("$name")
    fi
}

echo "=== KeySidekick Test Suite ==="
echo ""

echo "--- Core modules ---"
run_test input_ledger        "tests/input_ledger_tests.cpp src/input_ledger.cpp"
run_test report_diff         "tests/report_diff_tests.cpp"
run_test mingw_threading     "tests/mingw_threading_tests.cpp"
run_test http_security       "tests/http_security_tests.cpp src/http_security.cpp"        "-lbcrypt"
run_test domain_model        "tests/domain_model_tests.cpp src/domain_model.cpp"
run_test config_v3           "tests/config_v3_tests.cpp src/config_v3.cpp"                ""  "-O0"
run_test app_instance        "tests/app_instance_tests.cpp src/app_instance.cpp"
run_test windows_targets     "tests/windows_targets_tests.cpp src/windows_targets.cpp"    "-lgdi32 -luser32"
run_test command_queue       "tests/command_queue_tests.cpp"                              ""  "-O2"
run_test supervisor          "tests/supervisor_tests.cpp src/supervisor.cpp"
run_test runtime_storage     "tests/runtime_storage_tests.cpp src/runtime_storage.cpp"
run_test targeted_input      "tests/targeted_input_tests.cpp src/targeted_input.cpp"      "-luser32"

echo "--- Bridge + Parser ---"
run_test config_domain_bridge "tests/config_domain_bridge_tests.cpp src/config_domain_bridge.cpp src/config_v3.cpp src/domain_model.cpp"
run_test action_parser        "tests/action_parser_tests.cpp src/action_parser.cpp src/domain_model.cpp"

echo "--- Lifecycle modules ---"
run_test runtime_state       "tests/runtime_state_tests.cpp src/runtime_state.cpp src/mingw_threading.h" "" "-O2"

echo ""
# Гейт предупреждений для тестовых TU: флаги те же, что в build.bat. Предупреждение
# здесь — дефект либо теста, либо модуля, который он проверяет (именно так в
# продукте нашлась съеденная комментарием строка таблицы scan-кодов).
test_warnings=$(grep -h "warning:" "$OBJDIR"/*.build.log 2>/dev/null | head -20 || true)
if [ -n "$test_warnings" ]; then
    failed=$((failed+1))
    failed_names+=("compiler-warnings")
    echo "=== Test build warnings ==="
    echo "$test_warnings"
fi

echo "=== Results: $passed passed, $failed failed ==="

if [ $failed -gt 0 ]; then
    echo "Failed: ${failed_names[*]}"
    exit 1
fi
exit 0
