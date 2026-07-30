# Windows Wide Final Contract

이 corpus는 실제 compiler나 linker를 실행하지 않고 Windows/MSVC response-file
materialization을 검사한다. `tests/windows-prep.sh`가 임시 복사본에 `.obj` 입력을
생성한 뒤 1,000-object final action의 dry-run과 Ninja emission을 비교한다.
