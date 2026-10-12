#!/usr/bin/env bash
set -euo pipefail

PART="${1:-patch}"
VERSION_FILE="VERSION"

if [[ ! -f "$VERSION_FILE" ]]; then
    echo "0.1.0" > "$VERSION_FILE"
fi

CURRENT_VER=$(cat "$VERSION_FILE" | tr -d '[:space:]')
IFS='.' read -r MAJOR MINOR PATCH <<< "$CURRENT_VER"

case "$PART" in
    major)
        MAJOR=$((MAJOR + 1))
        MINOR=0
        PATCH=0
        ;;
    minor)
        MINOR=$((MINOR + 1))
        PATCH=0
        ;;
    patch)
        PATCH=$((PATCH + 1))
        ;;
    *)
        echo "Usage: $0 {major|minor|patch}"
        exit 1
        ;;
esac

NEW_VER="${MAJOR}.${MINOR}.${PATCH}"
echo "$NEW_VER" > "$VERSION_FILE"
echo "Version bumped: $CURRENT_VER -> $NEW_VER"

if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    # If initial commit hasn't been made yet, don't attempt commit
    if git rev-parse --verify HEAD >/dev/null 2>&1; then
        git add "$VERSION_FILE"
        git commit -m "chore(release): bump version to v$NEW_VER"
        git tag -a "v$NEW_VER" -m "Release v$NEW_VER"
        echo "Created git tag: v$NEW_VER"
    fi
fi
