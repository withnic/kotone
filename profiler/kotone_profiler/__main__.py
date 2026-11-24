"""Allow running via `python -m kotone_profiler`."""

import sys

from .cli import main

if __name__ == "__main__":
    sys.exit(main())
