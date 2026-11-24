PYTHON ?= python

.PHONY: help fmt lint test check clean

help:
	@echo Kotone (琴音) 開発タスク一覧:
	@echo   make fmt    - clang-format (C++) および ruff format/fix (Python) で整形
	@echo   make lint   - 静的規約監査 + ruff + clang-format --dry-run + clang-tidy を実行
	@echo   make test   - C++ (cmake/ctest) および Python (pytest) の全テストを実行
	@echo   make check  - lint + test を一括実行 (コミット・完了前チェック)
	@echo   make clean  - build/ 出力ディレクトリを削除

fmt:
	$(PYTHON) scripts/check.py fmt

lint:
	$(PYTHON) scripts/check.py lint

test:
	$(PYTHON) scripts/check.py test

check:
	$(PYTHON) scripts/check.py check

clean:
	$(PYTHON) -c "import shutil; shutil.rmtree('build', ignore_errors=True)"
