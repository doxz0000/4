/*
    MODEL-CLIPONIZER — DR.DOC SOUNDLAB-EQUIPMENT
    WebViewWarmup.h

    Прогрів WebView2: щойно плагін завантажено в DAW (ще до відкриття
    вікна), у фоні запускається браузерний процес msedgewebview2.exe.
    Коли користувач відкриває редактор, WebBrowserComponent з тією ж
    папкою даних перевикористовує вже запущений процес — UI з'являється
    значно швидше, ніж при холодному старті.

    Один спільний прогрітий WebView на всі екземпляри плагіна; він живе,
    поки існує хоча б один процесор.
*/

#pragma once

#include <JuceHeader.h>
#include <memory>

namespace cliponizer
{
    // Спільна папка даних WebView2 для редактора і прогріву. Має бути
    // однаковою, інакше WebView2 запустить окремий браузерний процес.
    juce::File getWebViewDataFolder();

    class WebViewWarmup
    {
    public:
        WebViewWarmup();
        ~WebViewWarmup();

        struct Impl;

    private:
        std::shared_ptr<Impl> impl;

        JUCE_DECLARE_NON_COPYABLE (WebViewWarmup)
    };
}
