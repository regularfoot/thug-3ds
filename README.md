# thug-3ds

An unofficial port of **Tony Hawk’s Underground** for the Nintendo 3DS. The current source build appears as **THUG1 0327** in the Homebrew Launcher. The original game loop runs on ARM11, the PICA200 renders the graphics, and the game reads its assets from the SD card.

This repository contains the 3DS port code and build tools. To build a playable copy, you need **both** a supported PC installation (its `Game/Data` folder) and a compatible source checkout of the original engine that you have permission to use. The retail PC installation supplies the game assets.

[Build and install in English](#build-and-install) · [Сборка и установка на русском](#сборка-и-установка)

## What changed in build 0315

- Solid elevator doors in Hawaii now hide the buildings behind them. The call-button panels keep their markings.
- The full lower façade and windows behind Moscow’s cannons are visible again. The renderer used to hide them when the camera faced the courtyard.
- The same material correction covers solid surfaces in Manhattan’s subway, the Hawaii hotel and Wallows, and the backstage corridor in Hotter Than Hell. Transparent glass keeps its original blending.
- The renderer also reuses static geometry while turning the camera in dense stereo views. Frame rate still depends on the level and camera direction.

## Camera and controls (introduced in 0.1.1)

| Console | Camera and controls |
|---|---|
| **New Nintendo 3DS / XL** | Both C-Stick axes are inverted. Use the physical ZL and ZR buttons as usual. |
| **Original Nintendo 3DS / XL (Old 3DS)** | Touch the center area of the lower screen and drag to turn the camera. Lift your finger to stop turning. The top row has ZL, ZL+ZR (both with one touch), and ZR. The bottom row has L3 and R3. You can hold physical buttons while using the touch controls. |

Both models have a **CFG** button at the bottom right of the touchscreen. Tap it to open the control settings; pause gameplay first. Use the D-pad to choose an action, press physical **A**, then press the physical button you want to assign. **Start** saves, **B** cancels changes, and **Select** restores the default layout. Assigning a button already in use swaps its action; an action may be unassigned if all physical buttons are in use. The mapping also applies to game menus. The classic layout remains the default. Settings are shared between builds in `/3ds/thug1-controls.cfg` and do not change game saves. On Old 3DS, the touch controls remain available.

Обе модели: нажмите **CFG** справа внизу сенсорного экрана, чтобы открыть настройки управления; перед этим поставьте игру на паузу. Крестовиной выберите действие, нажмите физическую **A**, затем нужную кнопку. **Start** сохраняет, **B** отменяет изменения, **Select** возвращает стандартную раскладку. Занятые кнопки меняются местами; действие может остаться неназначенным, если все физические кнопки заняты. Раскладка действует и в меню игры. Настройки общие для сборок: `/3ds/thug1-controls.cfg`; игровые сохранения не меняются. На Old 3DS сенсорные кнопки сохраняются.

This release includes a fix for music cutting out a few seconds into a song in Moscow. On Old 3DS, the port gives the engine more memory when loading levels while reserving memory for graphics and audio.

## Features and testing

- On a physical Old 3DS, build 0325 ran several levels. Sampled mono frame rates varied from roughly 18 FPS in New Jersey to 31 FPS in Florida. Stereo and dense scenes were slower.
- Build 0326 restores the SkitchNode used by Seattle's moving quarterpipe in Old 3DS Free Skate. The original movement script was exercised in Azahar. Hardware confirmation remains pending.

- Play Free Skate or Story, choose a skater, edit your skater and board, and save your progress. Stereoscopic 3D supports adjustable depth. A full Story playthrough on 0315 still needs testing.
- The Activision, Neversoft, and game intro movies play at startup. Press **A** or **B** to skip the current movie, or **Start** to skip the remaining intros. Use `--autoload` when preparing the SD folder to load a personal save at startup.
- On a physical **New 3DS**, build 0311 fixed the reported music dropouts. In Azahar, build 0312 completed 1,080 frames of skating in Moscow in stereo. On dense maps, frame rate varies with the direction the camera faces.
- With the **Old 3DS** profile in Azahar, build 0312 loaded the School II scene and its collision data. Build 0310 was also tested skating in School II and Venice with the Old 3DS profile. For a first run, try **Free Skate → School II** in mono. Memory limits which large levels can run on this model.
- For build 0315, the renderer was checked from five camera positions in Hawaii and five in Moscow in Azahar. The packaged build also reached gameplay in Moscow. All 20 host tests passed with AddressSanitizer and UndefinedBehaviorSanitizer, alongside 37 Python tests. The latest wall fixes await confirmation on physical hardware.

For smoother opening movies on Old 3DS, generate the optional converted videos from your own PC installation after preparing the SD folder. Install FFmpeg with Bink decoding support and run:

```sh
python3 tools/prepare_old_openings.py "/path/to/Game/Data/movies/bik" "out/3ds/thug1_source_engine_0327/data/movies/old"
```

The output directory must not already exist. The conversion can take time and requires additional storage. The game selects these files on Old 3DS.

## Build and install

The tested setup is macOS with Python **3.11+**, Git, Clang/Clang++, Make, CMake **3.20+**, Ninja, and Docker with Linux/amd64 support. On macOS, Apple’s Command Line Tools provide Clang and Make. Use a local filesystem for the source and temporary files, and give Docker access to the project directory. Run the commands below **from the root of this repository**. Initial tool installation and dependency downloads require internet access.

### 1. Prepare the engine and media sources

Point the first command at your authorized engine Git checkout containing `Code/` and `cmake_files.cmake`. Keep your unmodified PC `Game/Data` directory handy for step 3. The build tools check the inputs against these identifiers.

| Input | Expected Git object ID |
|---|---|
| `HEAD:Code` tree | `fcc181d6687e66a705ab0671187e81c0d5052232` |
| `HEAD:cmake_files.cmake` file | `a6ab241675c2c0f7fc595e6c941c5bed2fdcd0e1` |
| FFmpeg n7.1.1 | `db69d06eeeab4f46da15030a80d539efb4503ca8` |

```sh
python3 tools/prepare_engine.py "/path/to/authorized-engine-checkout"
python3 tools/media_codec.py fetch
```

The first command prepares a local copy of the engine sources with the 3DS changes applied. The second fetches FFmpeg from its official repository, verifies the revision, and prepares the Bink and WAV decoders.

### 2. Build the 3DS application and host tools

```sh
docker build --platform linux/amd64 -t rethpsu-sdk:0.1 .
docker run --rm --platform linux/amd64 --network none \
  -v "$PWD:/project" -w /project rethpsu-sdk:0.1 python3 build.py arm -j 4
python3 build.py host -j 4
```

The ARM build produces `build/arm/thug1_full_engine.3dsx` and `build/arm/thug1_full_engine.smdh`. The host build produces the tools that prepare textures, models, and level archives. The reference build used the included Dockerfile and the ARM command above with devkitARM r68, libctru 2.7.0, citro3d 1.7.1, and tex3ds 2.3.0. The Dockerfile uses the devkitPro base image, and the available package versions can change. [devkitPro Docker](https://github.com/devkitPro/docker)

### 3. Prepare the SD card folder

```sh
python3 make_sd.py --data "/path/to/THUG/Game/Data" --sdk-image rethpsu-sdk:0.1
```

The supported PC data directory contains 6,803 original files. The script verifies their contents, copies the assets, extracts the images needed by the skater editor, and prepares textures, simplified geometry, and smaller asset sets for Old 3DS. To carry over a save, add `--save-dir "/path/to/source-saves" --autoload "Kevin Baskin"` to the command and substitute your save’s exact in-game name. Each run needs a fresh output directory. If `out/3ds/thug1_source_engine_0327` already exists, move it aside first.

### 4. Install and play

Allow about **3 GB** of free space on a FAT32 SD card. Copy the entire `out/3ds/thug1_source_engine_0327` folder into `/3ds/` on the card and select **THUG1 0327** in the Homebrew Launcher. The startup log is written to `source_engine_boot.txt` in the application folder. When reporting a crash, include the console model, mono or stereo mode, level, and what you were doing just before it happened.

### Upgrading from an earlier build

Install `thug1_source_engine_0327` alongside the previous build. Copy your `source-saves/` folder and, if present, your `autoload_save.txt` into the new folder to keep your progress. Intro movies play by default. To skip them, place a file named `skip_opening.txt` in the new application folder.

## Сборка и установка

Для сборки нужны установленная PC-версия игры и отдельно совместимые исходники движка, которые вы вправе использовать.

Рабочая конфигурация — macOS с Python **3.11+**, Git, Clang/Clang++, Make, CMake **3.20+**, Ninja и Docker с поддержкой Linux/amd64. Для исходников и временных файлов используйте локальную файловую систему. На macOS инструменты командной строки Apple предоставляют Clang и Make. Команды ниже выполняются **из корня этого комплекта**. Docker должен видеть каталог проекта. Доступ к сети нужен при первой установке инструментов и получении открытых библиотек.

**1. Подготовьте входные файлы.** Укажите путь к Git-каталогу, где лежат `Code/` и `cmake_files.cmake`, и отдельно путь к установленной немодифицированной `Game/Data`. Сборщик проверяет точное содержимое входов. Проверенные идентификаторы Git приведены ниже.

| Вход | Идентификатор |
|---|---|
| Дерево `HEAD:Code` | `fcc181d6687e66a705ab0671187e81c0d5052232` |
| Файл `HEAD:cmake_files.cmake` | `a6ab241675c2c0f7fc595e6c941c5bed2fdcd0e1` |
| FFmpeg n7.1.1 | `db69d06eeeab4f46da15030a80d539efb4503ca8` |

```sh
python3 tools/prepare_engine.py "/path/to/authorized-engine-checkout"
python3 tools/media_codec.py fetch
```

Первая команда создаёт локальное представление исходников с правками для 3DS. Вторая получает FFmpeg из официального репозитория, проверяет ревизию и готовит декодеры Bink/WAV.

**2. Соберите программу и инструменты подготовки данных.**

```sh
docker build --platform linux/amd64 -t rethpsu-sdk:0.1 .
docker run --rm --platform linux/amd64 --network none \
  -v "$PWD:/project" -w /project rethpsu-sdk:0.1 python3 build.py arm -j 4
python3 build.py host -j 4
```

ARM-сборка создаёт `build/arm/thug1_full_engine.3dsx` и `build/arm/thug1_full_engine.smdh`. Сборка на компьютере готовит инструменты для текстур, моделей и архивов уровней. Версии SDK для контрольной сборки — devkitARM r68, libctru 2.7.0, citro3d 1.7.1 и tex3ds 2.3.0. Проверены сборка образа из прилагаемого Dockerfile, его версии SDK и ARM-команда из инструкции. Dockerfile использует базовый образ devkitPro. Содержимое пакетов в его репозитории может меняться. [devkitPro Docker](https://github.com/devkitPro/docker)

**3. Соберите папку для SD.**

```sh
python3 make_sd.py --data "/path/to/THUG/Game/Data" --sdk-image rethpsu-sdk:0.1
```

Проверенный каталог PC-игры содержит 6 803 исходных файла. Скрипт проверяет их содержимое, копирует ресурсы, извлекает недостающие изображения для редактора скейтера и готовит текстуры, упрощённую геометрию и наборы для Old 3DS. Для поддержки существующего прогресса добавьте к этой же команде параметры `--save-dir "/path/to/source-saves" --autoload "Kevin Baskin"`, подставив имя вашего сохранения из игры. Каждая сборка создаётся в новой папке. Если `out/3ds/thug1_source_engine_0327` уже существует, сначала переместите её в другое место.

Для более плавных вступительных роликов на Old 3DS можно после подготовки папки карты преобразовать свои PC-ролики. Установите FFmpeg с поддержкой Bink и выполните:

```sh
python3 tools/prepare_old_openings.py "/path/to/Game/Data/movies/bik" "out/3ds/thug1_source_engine_0327/data/movies/old"
```

Выходной папки до запуска быть не должно. Преобразование занимает время и требует дополнительного места.

**4. Запустите игру.** Скопируйте `out/3ds/thug1_source_engine_0327` в `/3ds/` на карте FAT32. Оставьте около **3 ГБ** свободного места. В Homebrew Launcher выберите **THUG1 0327**. Журнал запуска появляется рядом с игрой в `source_engine_boot.txt`. Для сообщения об ошибке укажите модель консоли, режим моно/стерео, уровень и действие перед сбоем.

### Если у вас уже есть предыдущая сборка

Установите папку `thug1_source_engine_0327` рядом с прежней. Чтобы продолжить игру, скопируйте свою папку `source-saves/` и, если он у вас есть, файл `autoload_save.txt` в папку 0327. Вступление включено по умолчанию. Файл `skip_opening.txt` в папке новой версии включает его пропуск.

## License and third-party components

The original 3DS port code and build tools in this repository are released under the MIT license. Keep the copyright notice and license text when redistributing those files. Third-party components retain their own licenses. [MIT license](LICENSE) · [Third-party notices](THIRD-PARTY.md)

The game title identifies compatibility. The original engine, game assets, and music remain the property of their respective rights holders. Distributing an executable that incorporates the original engine, or distributing game assets, requires a separate legal basis. This repository provides the port materials for local builds. [WIPO Copyright Treaty, Articles 4 and 6](https://www.wipo.int/treaties/en/ip/wct/) · [FFmpeg legal information](https://ffmpeg.org/legal.html)
