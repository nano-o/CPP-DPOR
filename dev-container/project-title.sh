# Set terminal title to the project name for interactive shells.
case "$-" in
  *i*) ;;
  *) return ;;
esac

set_title() {
  TITLE="${PROJECT_NAME:-${PWD##*/}}"
  export TITLE
  printf '\033]0;%s\007' "${TITLE}"
}

if [ -n "${PROMPT_COMMAND:-}" ]; then
  PROMPT_COMMAND="${PROMPT_COMMAND};set_title"
else
  PROMPT_COMMAND="set_title"
fi

if [ -n "${PS1:-}" ] && [ -z "${PROJECT_TITLE_SEQ:-}" ]; then
  PROJECT_TITLE_SEQ=1
  export PROJECT_TITLE_SEQ
  # Remove any existing title escape from PS1 so our title wins.
  PS1="${PS1//\\[\\e]0;*\\a\\]/}"
  PS1="${PS1}\[\e]0;\${TITLE}\a\]"
fi
