# Caller locale settings used by the R230-2 probes (sourced). Each value is a
# list of run.py --set arguments applied on top of run.py's base environment
# (LANG=en_US.UTF-8, LOCPATH=<scratch>/locales holding en_US, de_DE and fr_FR
# UTF-8 locales generated with localedef; LC_ALL, LANGUAGE, LC_MESSAGES unset).
declare -A SETTING=(
  [EN]=""
  [DE_LANGUAGE]="--set LANGUAGE=de"
  [FR_LANGUAGE]="--set LANGUAGE=fr"
  [DE_LANG]="--set LANG=de_DE.UTF-8"
  [FR_LANG]="--set LANG=fr_FR.UTF-8"
  [DE_LC_ALL]="--set LC_ALL=de_DE.UTF-8"
  [FR_LC_MESSAGES]="--set LC_MESSAGES=fr_FR.UTF-8"
  [MIXED]="--set LC_ALL=fr_FR.UTF-8 --set LANG=de_DE.UTF-8 --set LANGUAGE=de:fr --set LC_MESSAGES=de_DE.UTF-8"
)
ORDER="EN DE_LANGUAGE FR_LANGUAGE DE_LANG FR_LANG DE_LC_ALL FR_LC_MESSAGES MIXED"
RC=$WORKSPACE_HOME/milan-fpga-management/2026-09-22/100-r2-r230/receipts
RUN="python3 $RC/scripts/run.py"
S=/tmp/r230-100-r2-scratch
