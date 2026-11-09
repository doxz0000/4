cmake_minimum_required(VERSION 3.22)

project(ClipOnizer VERSION 1.0.3)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# JUCE: Підключаємо локальну папку замість викачування з інтернету
# ---------------------------------------------------------------------------
add_subdirectory("F:/SDK/JUCE" JUCE)

juce_add_plugin(ClipOnizer
    COMPANY_NAME                 "Dr.Doc SoundLab"
    BUNDLE_ID                    "com.drdoc.cliponizer"
    PLUGIN_MANUFACTURER_CODE     Drdc
    PLUGIN_CODE                  Clp8
    FORMATS                      VST3 AU Standalone
    PRODUCT_NAME                 "CLIPONIZER"
    IS_SYNTH                     FALSE
    NEEDS_MIDI_INPUT             FALSE
    NEEDS_MIDI_OUTPUT            FALSE
    IS_MIDI_EFFECT               FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS  FALSE
    COPY_PLUGIN_AFTER_BUILD      TRUE
    # NEEDS_WEB_BROWSER вмикає у JUCE все необхідне для juce::WebBrowserComponent
    # (на Windows це також автоматично тягне за собою WebView2 loader).
    # Ця властивість з'явилась у JUCE 8.0.2+. Якщо CMake видасть помилку про
    # "Unrecognized keyword" — приберіть цей рядок, залиште лише
    # juce::juce_gui_extra у target_link_libraries нижче, а на Windows
    # встановіть Microsoft Edge WebView2 SDK / Runtime вручну.
    NEEDS_WEB_BROWSER            TRUE
    NEEDS_WEBVIEW2               TRUE
)

# Генеруємо JuceHeader.h для компіляції проєкту
juce_generate_juce_header(ClipOnizer)

# Додаємо бінарні дані: старі картинки + нові файли Web UI (HTML/CSS/JS).
# Після кожної зміни ui.html / style.css / ui.js потрібен ПЕРЕБІЛД проєкту,
# бо ці файли компілюються всередину плагіна через BinaryData.
juce_add_binary_data(ClipOnizerResources
    SOURCES
        Resources/background.png
        Resources/knob.png
        Resources/DeltaOn.png
        Resources/DeltaOff.png
        WebUI/ui.html
        WebUI/style.css
        WebUI/ui.js
        WebUI/juce_interop.js
)

target_sources(ClipOnizer
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/WebViewWarmup.cpp
)

target_include_directories(ClipOnizer
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

target_compile_definitions(ClipOnizer
    PUBLIC
        JUCE_WEB_BROWSER=1
        JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0

    # ---- РЕЖИМ РОЗРОБКИ (опціонально) ------------------------------------
    # Розкоментуйте рядок нижче та вкажіть абсолютний шлях до папки WebUI,
    # щоб PluginEditor.cpp читав ui.html/style.css/ui.js напряму з диска.
    # Тоді для перегляду змін дизайну достатньо закрити й знову відкрити
    # вікно плагіна — БЕЗ перекомпіляції C++. Перед фінальною збіркою
    # (реліз/дистрибуція) обов'язково приберіть цей рядок, щоб файли
    # запікались у BinaryData і плагін був самодостатнім.
    # PRIVATE
    #     CLIPONIZER_WEBUI_DEV_DIR="F:/Projects/ClipOnizer/WebUI"
)

target_link_libraries(ClipOnizer
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_gui_extra
        ClipOnizerResources
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
