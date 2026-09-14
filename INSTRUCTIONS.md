# CLIPONIZER No.8 — перехід на HTML/CSS UI (JUCE 8 WebBrowserComponent)

## 1. Куди класти файли

Структура проєкту (замінює те, що у вас є зараз):

```
ВашПроєкт/
├── CMakeLists.txt              <- замінити
├── Resources/
│   ├── background.png          <- без змін
│   ├── knob.png                <- без змін
│   ├── DeltaOn.png              <- без змін
│   └── DeltaOff.png             <- без змін
├── Source/
│   ├── PluginProcessor.h        <- без функціональних змін (можна не чіпати)
│   ├── PluginProcessor.cpp      <- без функціональних змін (можна не чіпати)
│   ├── PluginEditor.h           <- ЗАМІНИТИ
│   └── PluginEditor.cpp         <- ЗАМІНИТИ
└── WebUI/                       <- НОВА папка, кладете сюди
    ├── ui.html                  <- дизайн/розмітка
    ├── style.css                <- ВЕСЬ дизайн/кольори/лейаут — редагуйте цей файл
    └── ui.js                    <- логіка інтерфейсу (ручки, малювання графіків)
```

`Source/PluginProcessor.h/.cpp` я не змінював по суті — весь DSP (обробка
звуку, кільцевий буфер осцилоскопа) лишається таким самим. Я віддав їх
повністю лише для повноти комплекту.

## 2. Що саме змінилось

- **Стара логіка**: `ClipShaperComponent`, `OscilloscopeComponent`,
  `ClipIndicatorComponent` і `ClipOnizerLookAndFeel` малювали все вручну
  через `juce::Graphics` (`paint()`).
- **Нова логіка**: у вікні плагіна тепер лежить один
  `juce::WebBrowserComponent`, що показує `ui.html`. Дизайн і візуал
  повністю в `style.css` (кольори, розміри, шрифти, лейаут) та `ui.html`
  (розмітка). `ui.js` містить інтерактив (drag ручок, кнопки, малювання
  графіків у `<canvas>`) і точну JS-копію функції `ClipShaper::process()`.
- Обмін даними:
  - **JS → C++**: коли ви крутите ручку/тиснете кнопку — JS викликає
    `callNative("setParam", id, value)`, це прилітає у
    `PluginEditor::jsSetParameter()` і виставляє параметр через APVTS
    (host automation працює нормально, є `beginGesture`/`endGesture`).
  - **C++ → JS**: 30 разів на секунду `PluginEditor::timerCallback()`
    збирає поточні значення параметрів, дані хвилі осцилоскопа (у
    стиснутому вигляді — 600 пар min/max), рівень кліпу, піки тощо, і
    викликає в браузері `window.__pluginUpdate(state)`.

## 3. Вимоги

- **JUCE 8.0.2 або новіша** (саме там з'явився стабільний
  `juce::WebBrowserComponent` з `withResourceProvider`/`withNativeFunction`
  та властивість `NEEDS_WEB_BROWSER` у `juce_add_plugin`). Якщо у вас
  старіша збірка JUCE — оновіть `F:/SDK/JUCE`.
- **Windows**: для `WebBrowserComponent` потрібен **Microsoft Edge
  WebView2 Runtime** на машині користувача (зазвичай вже стоїть у
  Windows 10/11) і WebView2 SDK на етапі збірки — `NEEDS_WEB_BROWSER TRUE`
  у CMake налаштовує це автоматично.
- **macOS**: використовується системний `WKWebView`, додаткового
  встановлювати не треба.

## 4. Кроки

1. Скопіюйте файли з цього комплекту у відповідні місця (див. структуру
   вище), замінивши старі `CMakeLists.txt`, `PluginEditor.h/.cpp`.
2. Створіть папку `WebUI/` поруч із `Source/` і покладіть туди
   `ui.html`, `style.css`, `ui.js`.
3. Видаліть стару CMake build-директорію (`build/`) або хоча б
   перезапустіть конфігурацію CMake — це важливо, бо `juce_add_binary_data`
   тепер отримав нові файли (html/css/js) і має перегенерувати `BinaryData`.
4. Зберіть проєкт як зазвичай.
5. Відкрийте плагін у хості — має з'явитись той самий дизайн (амбер/тьмяний,
   із ручками-картинками, осцилоскопом і clip-shaper графіком), але тепер
   намальований у вбудованому браузері.

## 5. Як редагувати дизайн

- Кольори, розміри, відступи, шрифти, розташування блоків — усе в
  **`WebUI/style.css`**. Змінні кольорів на початку файлу (`--textAmber`,
  `--traceHardClip` і т.д.) відповідають старим `ClipOnizerColours::...`.
- Розмітку (які блоки де стоять) — у **`WebUI/ui.html`**.
- ⚠️ Ці файли зараз "запікаються" у плагін через `BinaryData`, тому кожна
  зміна вимагає **перекомпіляції C++ проєкту**, щоб побачити результат.

### Швидка ітерація без перекомпіляції (опційно)

У `CMakeLists.txt` є закоментований блок:

```cmake
# PRIVATE
#     CLIPONIZER_WEBUI_DEV_DIR="F:/Projects/ClipOnizer/WebUI"
```

Розкоментуйте його і вкажіть абсолютний шлях до вашої папки `WebUI/`.
Тоді `PluginEditor.cpp` читатиме `ui.html`/`style.css`/`ui.js` прямо з
диска — досить закрити й знову відкрити вікно плагіна (без ребілда
C++), щоб побачити зміни дизайну. **Перед фінальною/релізною збіркою
обов'язково закоментуйте це знову** — інакше плагін не буде
самодостатнім (шукатиме файли на конкретному диску користувача).

## 6. Якщо щось не компілюється

Найімовірніші місця розбіжностей із вашою точною версією JUCE 8 (API
`WebBrowserComponent` є відносно новим і трохи змінювався між
мінорними релізами):

- `juce::WebBrowserComponent::Options::withResourceProvider(...)` —
  сигнатура callback (`std::optional<Resource>` vs інший тип).
- `juce::WebBrowserComponent::Options::withNativeFunction(...)` —
  назва типу `NativeFunctionCompletion`.
- `NEEDS_WEB_BROWSER TRUE` у `juce_add_plugin` — якщо CMake лається на
  невідомий аргумент, приберіть цей рядок.

Якщо компілятор вкаже на конкретний рядок — звірте його з офіційним
прикладом `examples/Plugins/WebViewPluginDemo` у вашій копії JUCE (він
іде в комплекті з SDK) і скоригуйте назву методу/типу — решта логіки
(native-функції, JSON пуш, canvas-рендер) міняти не потрібно.
