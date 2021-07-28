DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

activate_venv
PATH="$PATH:/data/multiversion"
$python buildscripts/resmoke.py generate-multiversion-exclude-tags --oldBinVersion=last_continuous
