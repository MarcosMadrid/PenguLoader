#include "browser.h"
#include <unordered_map>
#include "include/capi/cef_client_capi.h"
#include "include/capi/cef_urlrequest_capi.h"

// BROWSER PROCESS ONLY.

#ifndef OS_WIN
#define VK_OEM_PLUS     0xBB
#define VK_OEM_MINUS    0xBD
#endif

static std::unordered_map<int, void *> devtools_map_{};

static void enhance_devtools_window(void *handle)
{
    if (handle == nullptr) return;
#if OS_WIN
    extern HWND rclient_;
    HWND window = static_cast<HWND>(handle);

    // Get League icon.
    HICON icon_sm = (HICON)SendMessageW(rclient_, WM_GETICON, ICON_BIG, 0);
    HICON icon_bg = (HICON)SendMessageW(rclient_, WM_GETICON, ICON_BIG, 0);

    // Set window icon.
    SendMessageW(window, WM_SETICON, ICON_SMALL, (LPARAM)icon_sm);
    SendMessageW(window, WM_SETICON, ICON_BIG, (LPARAM)icon_bg);

    bool IsWindowsDarkTheme();
    void ForceDarkTheme(HWND);

    if (IsWindowsDarkTheme()) {
        // Force dark theme.
        ForceDarkTheme(window);
    }

    // Fix window content.
    RECT rc; GetClientRect(window, &rc);
    SetWindowPos(window, NULL, 0, 0,
        rc.right - 5, rc.bottom, SWP_NOMOVE | SWP_FRAMECHANGED);
#elif OS_MAC
    // TODO: fix window icon & title bar
#endif
}

struct DevToolsLifeSpan : CefRefCount<cef_life_span_handler_t>
{
    int parent_id_;

    DevToolsLifeSpan(int parent_id)
        : CefRefCount(this)
        , parent_id_(parent_id)
    {
        cef_bind_method(DevToolsLifeSpan, on_after_created);
        cef_bind_method(DevToolsLifeSpan, on_before_close);
    }

    void _on_after_created(cef_browser_t* browser)
    {
        auto host = browser->get_host(browser);

        // Save devtools handle.
        void *window = (void *)host->get_window_handle(host);
        devtools_map_.emplace(parent_id_, window);

        // Set initial zoom level
        double zoom_level = window::get_scaling(window) - 1.0;
        host->set_zoom_level(host, zoom_level);

        enhance_devtools_window(window);
        host->base.release(&host->base);
    };

    void _on_before_close(cef_browser_t* browser)
    {
        // Remove devtools handle.
        devtools_map_.erase(parent_id_);
    };
};

struct DevToolsKeyboardHandler : CefRefCount<cef_keyboard_handler_t>
{
    DevToolsKeyboardHandler() : CefRefCount(this)
    {
        cef_bind_method(DevToolsKeyboardHandler, on_pre_key_event);
    }

    int _on_pre_key_event(
        struct _cef_browser_t* browser,
        const struct _cef_key_event_t* event,
        cef_event_handle_t os_event,
        int* is_keyboard_shortcut)
    {
#if OS_MAC
        if (event->modifiers & EVENTFLAG_COMMAND_DOWN)
#else
        if (event->modifiers & EVENTFLAG_CONTROL_DOWN)
#endif
        {
            cef_browser_host_t *host = nullptr;

            if (event->windows_key_code == VK_OEM_PLUS)
            {
                host = browser->get_host(browser);
                host->set_zoom_level(host,
                    host->get_zoom_level(host) + 0.1);
            }
            else if (event->windows_key_code == VK_OEM_MINUS)
            {
                host = browser->get_host(browser);
                host->set_zoom_level(host,
                    host->get_zoom_level(host) - 0.1);
            }
            else if (event->windows_key_code == '0')
            {
                host = browser->get_host(browser);
                host->set_zoom_level(host, 0);
            }
#if OS_MAC  // Fix cut-copy-paste key bindings on macOS
            else if (event->windows_key_code == 'C' && event->focus_on_editable_field)
            {
                auto frame = browser->get_main_frame(browser);
                frame->copy(frame);
                frame->base.release(&frame->base);
                return true;
            }
            else if (event->windows_key_code == 'V' && event->focus_on_editable_field)
            {
                auto frame = browser->get_main_frame(browser);
                frame->paste(frame);
                frame->base.release(&frame->base);
                return true;
            }
            else if (event->windows_key_code == 'X' && event->focus_on_editable_field)
            {
                auto frame = browser->get_main_frame(browser);
                frame->cut(frame);
                frame->base.release(&frame->base);
                return true;
            }
#endif

            if (host != nullptr)
            {
                host->base.release(&host->base);
                return true;
            }
        }

        return false;
    };
};

struct DevToolsClient : CefRefCount<cef_client_t>
{
    int parent_id_;

    DevToolsClient(int parent_id)
        : CefRefCount(this)
        , parent_id_(parent_id)
    {
        cef_bind_method(DevToolsClient, get_life_span_handler);
        cef_bind_method(DevToolsClient, get_keyboard_handler);
    }

    cef_life_span_handler_t *_get_life_span_handler()
    {
        return new DevToolsLifeSpan(parent_id_);
    }

    cef_keyboard_handler_t *_get_keyboard_handler()
    {
        return new DevToolsKeyboardHandler();
    }
};

void browser::open_devtools(cef_browser_t *browser)
{
    int browser_id = browser->get_identifier(browser);
    const auto &it = devtools_map_.find(browser_id);

    // Found existing devtools window.
    if (it != devtools_map_.end())
    {
        window::make_foreground(it->second);
    }
    else
    {
        auto host = browser->get_host(browser);
        auto hview = host->get_window_handle(host);

        cef_window_info_t wi{};
#if OS_WIN
        wi.ex_style = WS_EX_APPWINDOW;
        wi.style = WS_OVERLAPPEDWINDOW
            | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_VISIBLE;
#endif
        // position next to the client window
        window::get_rect(hview, &wi.x, &wi.y, &wi.width, &wi.height);
        wi.x += 100;
        wi.y += 100;

        char caption[512];
        strcpy(caption, "League Client DevTools - ");
        auto frame = browser->get_focused_frame(browser);
        CefScopedStr url{ frame->get_url(frame) };

        if (url.empty())
            strcat(caption, url.to_utf8().c_str());
        else
            strcat(caption, "about:blank");
        wi.window_name = CefStr(caption, strlen(caption)).forward();

        cef_browser_settings_t settings{};
        host->show_dev_tools(host, &wi, new DevToolsClient(browser_id), &settings, nullptr);
        //                              ^--- We use new client to keep DevTools
        //                                   from being scaled by League Client (e.g 0.8, 1.6).

        frame->base.release(&frame->base);
        host->base.release(&host->base);
    }
}

// void OpenRemoteDevTools()
// {
//     if (remote_debugging_port_ != 0
//         && !remote_devtools_url_.empty())
//     {
//         shell::open_url(remote_devtools_url_.c_str());
//     }
// }

// void PrepareRemoteDevTools()
// {
//     struct RequestClient : CefRefCount<cef_urlrequest_client_t>
//     {
//         cef_urlrequest_t *url_request_;
//         str data_;

//         RequestClient() : CefRefCount(this), url_request_(nullptr), data_{}
//         {
//             cef_urlrequest_client_t::on_request_complete = on_request_complete;
//             cef_urlrequest_client_t::on_download_data = on_download_data;
//         }

//         ~RequestClient()
//         {
//             if (url_request_ != nullptr)
//                 url_request_->base.release(&url_request_->base);
//         }

//         void request()
//         {
//             str url{ "http://127.0.0.1:" };
//             url.append(std::to_string(remote_debugging_port_));
//             url.append("/json/list");

//             auto request_ = cef_request_create();
//             request_->set_url(request_, &CefStr(url));

//             url_request_ = cef_urlrequest_create(request_, this, nullptr);
//         }

//         static void CEF_CALLBACK on_request_complete(struct _cef_urlrequest_client_t* _,
//             struct _cef_urlrequest_t* request)
//         {
//             auto self = static_cast<RequestClient *>(_);

//             auto response = self->url_request_->get_response(self->url_request_);
//             auto status = self->url_request_->get_request_status(self->url_request_);

//             if (status == UR_SUCCESS && response->get_status(response) == 200)
//             {
//                 const char pattern[] = "\"devtoolsFrontendUrl\": \"";
//                 if (auto pos = strstr(self->data_.c_str(), pattern))
//                 {
//                     auto start = pos + sizeof(pattern) - 1;
//                     auto end = strstr(start, "\"");

//                     str link = "http://127.0.0.1:";
//                     link.append(std::to_string(remote_debugging_port_));
//                     link.append(str(start, end - start));

//                     remote_devtools_url_ = wstr(link.begin(), link.end());
//                 }
//             }

//             response->base.release(&response->base);
//         }

//         static void CEF_CALLBACK on_download_data(struct _cef_urlrequest_client_t* _,
//             struct _cef_urlrequest_t* request,
//             const void* data,
//             size_t data_length)
//         {
//             auto self = static_cast<RequestClient *>(_);
//             self->data_.append(static_cast<const char *>(data), data_length);
//         }
//     };

//     if (remote_debugging_port_ != 0)
//     {
//         auto client = new RequestClient();
//         client->request();
//     }
// }