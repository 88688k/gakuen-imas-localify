import os, json, shutil
import posixpath
from imas_tools.story.adapter import (
    merge_translated_csv_into_txt,
    trivial_translation_merger,
    line_level_dual_lang_translation_merger
)

def merge_translation_files(raw_folder: str, translation_folder: str, pretranslation_folder: str, resource_folder: str):
    try:
        translation_file_index = json.load(
            open(os.path.join(pretranslation_folder, "index.json"), encoding="utf-8")
        )
        print(f"Loaded index from {pretranslation_folder}/index.json")
    except Exception as e:
        print(f"Failed to load index.json from {pretranslation_folder}: {e}")
        return

    for k in translation_file_index:
        translation_file_index[k] = posixpath.join(
            pretranslation_folder, translation_file_index[k]
        )

    try:
        with open(
            os.path.join(translation_folder, "index.json"), "r", encoding="utf-8"
        ) as f:
            tmp = json.load(f)
            for k in tmp:
                translation_file_index[k] = posixpath.join(translation_folder, tmp[k])
        print(f"Updated index from {translation_folder}/index.json")
    except Exception as e:
        print(f"Failed to load or update index.json from {translation_folder}: {e}")
        return

    if not os.path.exists(resource_folder):
        os.makedirs(resource_folder)

    for file in os.listdir(raw_folder):
        if not file.endswith(".txt"):
            print(f"Skipping non-TXT file: {file}")
            continue

        translation_file_path = translation_file_index.get(file)
        if translation_file_path is None:
            print(f"No translation file for {file}")
            continue

        try:
            with open(translation_file_path, "r", encoding="utf-8") as f:
                csv = "".join(f.readlines())
            print(f"Loaded CSV file for {file} from {translation_file_path}")

            raw_file_path = posixpath.join(raw_folder, file)
            with open(raw_file_path, "r", encoding="utf-8") as f:
                txt = "".join(f.readlines())
            print(f"Loaded TXT file: {file} from {raw_file_path}")

            dest_resource_path = posixpath.join(resource_folder, file)

            try:
                merged_txt = merge_translated_csv_into_txt(
                    csv, txt, line_level_dual_lang_translation_merger
                )
                with open(dest_resource_path, "w", encoding="utf-8") as f:
                    f.write(merged_txt)
                print(f"Merged and saved {file} to {dest_resource_path}")
            except Exception as merge_exception:
                print(f"Merge error for {file}: {merge_exception}")
                print(f"Original TXT content:\n{txt}")
                print(f"CSV content:\n{csv}")

        except Exception as e:
            print(f"Failed to merge or save {file}: {e}")

if __name__ == "__main__":
    raw_folder = "./raw"
    translation_folder = "./gakuen-adapted-translation-data"
    pretranslation_folder = "./GakumasPreTranslation"
    resource_folder = "./local-files/resource"
    merge_translation_files(raw_folder, translation_folder, pretranslation_folder, resource_folder)
    
    try:
        shutil.copy(
            f"{pretranslation_folder}/etc/localization.json",
            f"{resource_folder}/localization.json",
        )
        print(f"Copied localization.json to {resource_folder}/localization.json")
    except Exception as e:
        print(f"Failed to copy localization.json: {e}")
