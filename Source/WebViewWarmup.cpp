/*
    MODEL-CLIPONIZER — DR.DOC SOUNDLAB-EQUIPMENT
    WebViewWarmup.cpp
*/

#include "WebViewWarmup.h"
#include <mutex>

#if JUCE_WINDOWS && JUCE_WEB_BROWSER && JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING
 #define CLIPONIZER_WEBVIEW2_WARMUP 1
#else
 #define CLIPONIZER_WEBVIEW2_WARMUP 0
#endif

#if CLIPONIZER_WEBVIEW2_WARMUP
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>

 JUCE_BEGIN_IGNORE_WARNINGS_MSVC (4265)
 #include <wrl.h>
 JUCE_END_IGNORE_WARNINGS_MSVC

 #include "WebView2.h"

 JUCE_BEGIN_IGNORE_WARNINGS_MSVC (4458)
 #include "WebView2EnvironmentOptions.h"
 JUCE_END_IGNORE_WARNINGS_MSVC
#endif

namespace cliponizer
{

// Постійна папка даних WebView2 (кеш, профіль). Спільна для всіх
// екземплярів плагіна і для прогріву — WebView2 перевикористовує один
// браузерний процес лише для однакової папки та однакових опцій.
juce::File getWebViewDataFolder()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("Dr.Doc SoundLab")
                   .getChildFile ("ClipOnizer")
                   .getChildFile ("WebView2");

    if (! dir.isDirectory() && ! dir.createDirectory())
        return juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("ClipOnizer_WebView2");

    return dir;
}

//==============================================================================
#if CLIPONIZER_WEBVIEW2_WARMUP

struct WebViewWarmup::Impl : public std::enable_shared_from_this<Impl>
{
    ~Impl()
    {
        if (controller != nullptr)
            controller->Close();

        controller = nullptr;
        environment = nullptr;

        if (hiddenWindow != nullptr)
            DestroyWindow (hiddenWindow);
    }

    // Викликається на message thread (WebView2 вимагає STA-потік з циклом повідомлень).
    void start()
    {
        using namespace Microsoft::WRL;

        // Ті самі опції, що використовує JUCE у WebBrowserComponent, —
        // інакше друге середовище з тією ж папкою не створиться.
        auto options = Make<CoreWebView2EnvironmentOptions>();
        const auto folder = getWebViewDataFolder().getFullPathName();
        std::weak_ptr<Impl> weak = weak_from_this();

        CreateCoreWebView2EnvironmentWithOptions (nullptr,
                                                  folder.toWideCharPointer(),
                                                  options.Get(),
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> (
                [weak] (HRESULT hr, ICoreWebView2Environment* env) -> HRESULT
                {
                    if (auto self = weak.lock())
                        if (SUCCEEDED (hr) && env != nullptr)
                            self->createController (env);

                    return S_OK;
                }).Get());
    }

    // Сам environment браузерний процес ще не запускає — його запускає
    // створення контролера. Тому створюємо невидимий контролер у прихованому
    // вікні (без навігації) і тримаємо його, поки живі екземпляри плагіна.
    void createController (ICoreWebView2Environment* env)
    {
        using namespace Microsoft::WRL;

        environment = env;

        hiddenWindow = CreateWindowExW (WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP,
                                        0, 0, 1, 1, nullptr, nullptr, nullptr, nullptr);

        if (hiddenWindow == nullptr)
            return;

        std::weak_ptr<Impl> weak = weak_from_this();

        env->CreateCoreWebView2Controller (hiddenWindow,
            Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> (
                [weak] (HRESULT hr, ICoreWebView2Controller* newController) -> HRESULT
                {
                    auto self = weak.lock();

                    if (SUCCEEDED (hr) && newController != nullptr)
                    {
                        if (self == nullptr)
                        {
                            newController->Close();   // плагін вже вивантажили
                            return S_OK;
                        }

                        newController->put_IsVisible (FALSE);
                        self->controller = newController;
                    }

                    return S_OK;
                }).Get());
    }

    Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller>  controller;
    HWND hiddenWindow = nullptr;
};

#else

struct WebViewWarmup::Impl
{
    void start() {}
};

#endif

//==============================================================================
namespace
{
    std::mutex sharedWarmupLock;
    std::weak_ptr<WebViewWarmup::Impl> sharedWarmup;
}

WebViewWarmup::WebViewWarmup()
{
    const std::lock_guard<std::mutex> lock (sharedWarmupLock);

    impl = sharedWarmup.lock();

    if (impl != nullptr)
        return;

    impl = std::make_shared<Impl>();
    sharedWarmup = impl;

    std::weak_ptr<Impl> weak = impl;
    juce::MessageManager::callAsync ([weak]
    {
        if (auto self = weak.lock())
            self->start();
    });
}

WebViewWarmup::~WebViewWarmup()
{
    // COM-об'єкти WebView2 і вікно треба звільняти на message thread.
    auto* mm = juce::MessageManager::getInstanceWithoutCreating();

    if (mm != nullptr && ! mm->isThisTheMessageThread())
        juce::MessageManager::callAsync ([toRelease = std::move (impl)]() mutable { toRelease.reset(); });
}

} // namespace cliponizer
