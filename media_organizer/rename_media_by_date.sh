#!/bin/bash

# To inherit all metadata from file to file
# exiftool -TagsFromFile source.file "-all:all>all:all" target.file

# Get common media creation date tags
# exiftool -CreateDate -ModifyDate -TrackCreateDate -TrackModifyDate -MediaCreateDate -MediaModifyDate source.file

# Write to common media creation date tags
# exiftool -CreateDate="" -ModifyDate="" -TrackCreateDate="" -TrackModifyDate="" -MediaCreateDate="" -MediaModifyDate="" source.file

DESTINATION_DIR="$PWD/output"
OIFS="$IFS"
IFS=$'\n'

# Create output directory if it doesn't exist
if [ ! -d "$DESTINATION_DIR" ]; then
  mkdir -p "$DESTINATION_DIR"
fi

# Supported file extensions
for FILE in *; do
  EXT="${FILE##*.}"

  # Skip if not a supported media type
  case "$EXT" in
    jpg|JPG|jpeg|JPEG|png|PNG|heic|HEIC|dng|DNG|mov|MOV|mp4|MP4|m4v|M4V|3gp|3GP) ;;
    *) continue ;;
  esac

  # Try DateTimeOriginal
  DATE=$(exiftool -quiet -dateformat "%Y-%m-%d--%H-%M-%S" -json -DateTimeOriginal "$FILE" | jq -r '.[0].DateTimeOriginal // empty')

  # Try CreationDate (for some videos)
  if [ -z "$DATE" ]; then
    DATE=$(exiftool -quiet -dateformat "%Y-%m-%d--%H-%M-%S" -json -CreationDate "$FILE" | jq -r '.[0].CreationDate // empty')
  fi

  # Try MediaCreateDate (for others)
  if [ -z "$DATE" ]; then
    DATE=$(exiftool -quiet -dateformat "%Y-%m-%d--%H-%M-%S" -json -MediaCreateDate "$FILE" | jq -r '.[0].MediaCreateDate // empty')
  fi

  # Try XMP-photoshop:DateCreated (for PNG, PSD, etc.)
  if [ -z "$DATE" ]; then
    DATE=$(exiftool -quiet -dateformat "%Y-%m-%d--%H-%M-%S" -json -XMP-photoshop:DateCreated "$FILE" | jq -r '.[0]["DateCreated"] // empty')
  fi

  # If we found a date, rename and move
  if [ -n "$DATE" ]; then
    FILENAME="${DATE}.${EXT}"
    DEST_PATH="$DESTINATION_DIR/$FILENAME"

    # If the timestamp collides with an existing file (e.g. burst shots,
    # or cameras with 1-second date resolution), append -1, -2, ... until unique.
    if [ -e "$DEST_PATH" ]; then
      SUFFIX=1
      while [ -e "$DESTINATION_DIR/${DATE}-${SUFFIX}.${EXT}" ]; do
        SUFFIX=$((SUFFIX + 1))
      done
      FILENAME="${DATE}-${SUFFIX}.${EXT}"
      DEST_PATH="$DESTINATION_DIR/$FILENAME"
    fi

    mv "$FILE" "$DEST_PATH"
    echo "✅ Moved: $FILENAME"
  else
    echo "❌ $FILE doesn't have EXIF or date metadata."
  fi
done

IFS="$OIFS"
