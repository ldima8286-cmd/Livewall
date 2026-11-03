# LiveWall

Живые обои для рабочего стола Windows: GIF, зацикленное видео и статичные
изображения рисуются в слое обоев Explorer, под иконками, без оверлея поверх
рабочего стола.

```
┌──────────────────────────────────────────────┐
│ Progman                                      │
│  └── SHELLDLL_DefView  (иконки, остаются сверху)
│ WorkerW               (наш слой, под иконками)
│  └── LiveWallDesktopWindow  ×  монитор       │
│       └── D3D11 swap chain                   │
└──────────────────────────────────────────────┘
```

## Что уже работает

- **GIF** и **статичные изображения** (PNG/JPG/BMP/TIFF/ICO) через Windows
  Imaging Component, с учётом задержек кадров.
- **Видео** через Media Foundation: MP4/M4V, WebM, MKV, MOV, AVI, WMV —
  всё, для чего в системе есть декодер. Видео зацикливается, звук не играет.
- **Несколько мониторов**, отдельное окно на каждый, с учётом per-monitor DPI.
- Режимы масштабирования: `fit` (вписать с полями), `fill` (заполнить с
  обрезкой), `stretch`.
- **Трей**: открыть файл, пауза, режим, автозапуск, перезапуск обоев, журнал,
  выход.
- **Устойчивость**: переживает перезапуск Explorer, смену конфигурации
  мониторов, потерю D3D-устройства.
- **Экономия**: пауза при блокировке сессии, в полноэкранных приложениях
  (играх), опционально от батареи.
- Настройки в `%LOCALAPPDATA%\LiveWall\settings.ini`, журнал в
  `%LOCALAPPDATA%\LiveWall\livewall.log`.

## Требования

- Windows 10/11 x64.
- Сборка: Visual Studio 2022 (MSVC) + CMake ≥ 3.20, либо MinGW-w64 (UCRT) +
  CMake + Ninja.
- Никаких сторонних библиотек: только Windows SDK.

## Сборка

Есть готовые пресеты (`CMakePresets.json`):

```bash
cmake --preset mingw && cmake --build --preset mingw        # или: --preset msvc
```

### MSVC

```bat
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Готовый файл: `build\Release\livewall.exe` (и копия в `build\dist`).

### MinGW-w64 (в том числе кросс-компиляция с Linux)

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake
cmake --build build
```

Бинарник статически слинкован с `libgcc`/`libstdc++`, внешних DLL не требует.

### Отладочная консольная сборка

```bash
cmake -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DLIVEWALL_CONSOLE=ON
```

## Запуск

```bat
livewall.exe "D:\Wallpapers\loop.mp4"
livewall.exe "D:\Wallpapers\cat.gif" --mode fill --fps 30
livewall.exe --autostart
```

| Аргумент | Значение |
|---|---|
| `<путь>` | Файл обоев (перекрывает сохранённый) |
| `--mode fit\|fill\|stretch` | Режим масштабирования |
| `--fps <1..240>` | Ограничение частоты кадров, по умолчанию 60 |
| `--autostart` | Служебный флаг для автозапуска (ничего не делает) |

Повторный запуск с путём передаёт файл уже работающему экземпляру и выходит.

## Ограничения

- `WorkerW`/`0x052C` — **недокументированный** механизм. Microsoft меняла его
  между сборками Windows и может сломать снова. В коде есть несколько
  стратегий поиска слоя, но 100% гарантии нет.
- Видео декодируется в память (RGB32) и заливается в текстуру — надёжно и
  просто, но на 4K/60 ГГц-дорогих сценариях это лишние ~8 МБ копирования на
  кадр. Аппаратный DXVA-путь — в планах.
- HEVC/H.265, VP9 и AV1 работают только если в системе установлены
  соответствующие кодеки (в Windows 11 HEVC — отдельное платное расширение).
- HDR-контент не обрабатывается: кадры приводятся к 8-битному BGRA.
- Нет GUI для выбора файла/настроек — только трей и INI.
- Звук видео намеренно отключён.

## Диагностика

1. Обои не появились или чёрный экран — откройте журнал из трея
   («Открыть журнал») или `%LOCALAPPDATA%\LiveWall\livewall.log`.
2. `WorkerW not found` — Explorer ещё не поднялся либо сборка Windows не
   поддерживает механизм. Проверьте, что иконки рабочего стола включены.
3. Видео не открывается — проверьте, воспроизводится ли файл в «Films & TV».
   Если нет, в системе нет декодера.
4. Сбросить настройки: удалить `%LOCALAPPDATA%\LiveWall\settings.ini`.

## Документация

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — как устроено внутри.
- [`docs/ROADMAP.md`](docs/ROADMAP.md) — что дальше.
- [`docs/TESTING.md`](docs/TESTING.md) — ручной тест-план под Windows.

## Лицензия

MIT, см. [`LICENSE`](LICENSE).
