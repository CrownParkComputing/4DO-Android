#!/usr/bin/env bash
set -euo pipefail

: "${PLAY_ACCESS_TOKEN:?PLAY_ACCESS_TOKEN is required}"

package_name="com.fourdo.android"
api_root="https://androidpublisher.googleapis.com/androidpublisher/v3/applications/${package_name}"
upload_root="https://androidpublisher.googleapis.com/upload/androidpublisher/v3/applications/${package_name}"
auth_header="Authorization: Bearer ${PLAY_ACCESS_TOKEN}"
committed=false

edit_json=$(curl --fail --silent --show-error \
  --request POST \
  --header "$auth_header" \
  --header 'Content-Type: application/json' \
  --data '{}' \
  "${api_root}/edits")
edit_id=$(jq --exit-status --raw-output '.id' <<<"$edit_json")

cleanup_edit() {
  if [ "$committed" != true ]; then
    curl --silent --request DELETE --header "$auth_header" \
      "${api_root}/edits/${edit_id}" >/dev/null || true
  fi
}
trap cleanup_edit EXIT

# Keep every existing localisation intact while replacing the old product name.
existing=$(curl --fail --silent --show-error \
  --header "$auth_header" \
  "${api_root}/edits/${edit_id}/listings")
while IFS= read -r language; do
  [ -n "$language" ] || continue
  body=$(jq --compact-output --arg language "$language" \
    '.listings[] | select(.language == $language) | .title = "Retro-3DO"' \
    <<<"$existing")
  curl --fail --silent --show-error \
    --request PUT \
    --header "$auth_header" \
    --header 'Content-Type: application/json' \
    --data "$body" \
    "${api_root}/edits/${edit_id}/listings/${language}" >/dev/null
done < <(jq --raw-output '.listings[]?.language' <<<"$existing")

update_english_listing() {
  local language="$1"
  local listing_dir="play/listings/${language}"
  local title short_description full_description body
  title=$(<"${listing_dir}/title.txt")
  short_description=$(<"${listing_dir}/short-description.txt")
  full_description=$(<"${listing_dir}/full-description.txt")
  body=$(jq --null-input --compact-output \
    --arg language "$language" \
    --arg title "$title" \
    --arg shortDescription "$short_description" \
    --arg fullDescription "$full_description" \
    '{language: $language, title: $title, shortDescription: $shortDescription, fullDescription: $fullDescription}')
  curl --fail --silent --show-error \
    --request PUT \
    --header "$auth_header" \
    --header 'Content-Type: application/json' \
    --data "$body" \
    "${api_root}/edits/${edit_id}/listings/${language}" >/dev/null
}

replace_images() {
  local language="$1"
  local image_type="$2"
  shift 2

  curl --fail --silent --show-error \
    --request DELETE \
    --header "$auth_header" \
    "${api_root}/edits/${edit_id}/listings/${language}/${image_type}" >/dev/null

  local image
  for image in "$@"; do
    curl --fail --silent --show-error \
      --request POST \
      --header "$auth_header" \
      --header 'Content-Type: image/png' \
      --data-binary "@${image}" \
      "${upload_root}/edits/${edit_id}/listings/${language}/${image_type}?uploadType=media" >/dev/null
  done
}

screenshots=(play/graphics/phone/*.png)
for language in en-US en-GB; do
  update_english_listing "$language"
  replace_images "$language" icon play/graphics/store/icon.png
  replace_images "$language" featureGraphic play/graphics/store/feature-graphic.png
  replace_images "$language" phoneScreenshots "${screenshots[@]}"
done

curl --fail --silent --show-error \
  --request POST \
  --header "$auth_header" \
  --header 'Content-Type: application/json' \
  --data '{}' \
  "${api_root}/edits/${edit_id}:commit" >/dev/null
committed=true
echo "Committed Play listing edit ${edit_id} for ${package_name}"
