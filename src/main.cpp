#include "transfer_worker.h"

#include "save_transfer_metadata.h"

#include <wx/wx.h>
#include <wx/filedlg.h>
#include <wx/gauge.h>
#include <wx/mstream.h>
#include <wx/timer.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#endif

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace
{
void CopyToBuffer(char* destination, size_t size, const wxString& text)
{
    const wxScopedCharBuffer utf8 = text.ToUTF8();
    std::snprintf(destination, size, "%s", utf8.data() ? utf8.data() : "");
}

std::string ToUtf8(const wxString& text)
{
    const wxScopedCharBuffer utf8 = text.ToUTF8();
    return utf8.data() ? std::string(utf8.data()) : std::string();
}

bool ReadBinaryFile(const std::string& path, std::vector<uint8_t>& output)
{
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    if (!file)
        return false;
    output.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return file.good() || file.eof();
}

class WinsockScope
{
public:
    WinsockScope()
    {
#ifdef _WIN32
        WSADATA data = {};
        ok_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
        ok_ = true;
#endif
    }

    ~WinsockScope()
    {
#ifdef _WIN32
        if (ok_)
            WSACleanup();
#endif
    }

    bool ok() const { return ok_; }

private:
    bool ok_ = false;
};

class MainFrame final : public wxFrame
{
public:
    static constexpr int kLogTimerId = wxID_HIGHEST + 1;

    MainFrame()
        : wxFrame(nullptr, wxID_ANY, "MCU Save Transfer",
                  wxDefaultPosition, wxSize(820, 560)),
          timer_(this, kLogTimerId)
    {
        Log(state_, "ready");

        wxPanel* panel = new wxPanel(this);
        wxBoxSizer* root = new wxBoxSizer(wxVERTICAL);

        wxStaticText* title = new wxStaticText(panel, wxID_ANY, "MCU Save Transfer");
        wxFont title_font = title->GetFont();
        title_font.SetPointSize(title_font.GetPointSize() + 4);
        title_font.SetWeight(wxFONTWEIGHT_BOLD);
        title->SetFont(title_font);
        root->Add(title, 0, wxALL, 8);

        wxFlexGridSizer* form = new wxFlexGridSizer(4, 3, 8, 8);
        form->AddGrowableCol(1, 1);
        form->Add(new wxStaticText(panel, wxID_ANY, "Switch discovery"), 0,
                  wxALIGN_CENTER_VERTICAL);
        switch_discovery_ = new wxChoice(panel, wxID_ANY);
        switch_discovery_->Append("Auto");
        switch_discovery_->Append("Manual IP");
        switch_discovery_->SetSelection(state_.auto_detect_switch ? 0 : 1);
        form->Add(switch_discovery_, 1, wxEXPAND);
        form->AddSpacer(1);
        AddTextRow(panel, form, "Switch IPv4", switch_ip_, state_.remote_host);
        AddFileRow(panel, form, "World File", file_a_, state_.send_path_a);
        AddFileRow(panel, form, ".ext File", file_b_, state_.send_path_b);
        root->Add(form, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

        wxStaticBoxSizer* ext_box = new wxStaticBoxSizer(wxVERTICAL, panel, ".ext metadata");
        wxFlexGridSizer* metadata = new wxFlexGridSizer(4, 2, 6, 8);
        metadata->AddGrowableCol(1, 1);
        metadata->Add(new wxStaticText(panel, wxID_ANY, "World name"), 0, wxALIGN_CENTER_VERTICAL);
        world_name_ = new wxTextCtrl(panel, wxID_ANY, "");
        metadata->Add(world_name_, 1, wxEXPAND);
        metadata->Add(new wxStaticText(panel, wxID_ANY, "Save version"), 0, wxALIGN_CENTER_VERTICAL);
        version_label_ = new wxStaticText(panel, wxID_ANY, "-");
        metadata->Add(version_label_, 1, wxALIGN_CENTER_VERTICAL);
        metadata->Add(new wxStaticText(panel, wxID_ANY, "Seed"), 0, wxALIGN_CENTER_VERTICAL);
        seed_label_ = new wxStaticText(panel, wxID_ANY, "-");
        metadata->Add(seed_label_, 1, wxALIGN_CENTER_VERTICAL);
        metadata->Add(new wxStaticText(panel, wxID_ANY, "Host options / extra data"), 0,
                      wxALIGN_CENTER_VERTICAL);
        options_label_ = new wxStaticText(panel, wxID_ANY, "-");
        metadata->Add(options_label_, 1, wxALIGN_CENTER_VERTICAL);
        ext_box->Add(metadata, 0, wxEXPAND | wxALL, 8);

        wxBoxSizer* icon_row = new wxBoxSizer(wxHORIZONTAL);
        icon_bitmap_ = new wxStaticBitmap(panel, wxID_ANY, wxBitmap(64, 64));
        import_icon_button_ = new wxButton(panel, wxID_ANY, "Import icon PNG");
        imported_icon_label_ = new wxStaticText(panel, wxID_ANY, "");
        icon_row->Add(icon_bitmap_, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 8);
        icon_row->Add(import_icon_button_, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 8);
        icon_row->Add(imported_icon_label_, 1, wxALIGN_CENTER_VERTICAL);
        ext_box->Add(icon_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        root->Add(ext_box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

        wxBoxSizer* actions = new wxBoxSizer(wxHORIZONTAL);
        send_button_ = new wxButton(panel, wxID_ANY, "Send");
        progress_ = new wxGauge(panel, wxID_ANY, 100, wxDefaultPosition,
                                wxSize(220, -1));
        status_ = new wxStaticText(panel, wxID_ANY, "");
        actions->Add(send_button_, 0, wxRIGHT, 8);
        actions->Add(progress_, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 8);
        actions->Add(status_, 1, wxALIGN_CENTER_VERTICAL);
        root->Add(actions, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

        root->Add(new wxStaticText(panel, wxID_ANY, "Debug log"), 0,
                  wxLEFT | wxRIGHT | wxBOTTOM, 8);
        log_ = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                              wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2);
        root->Add(log_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

        panel->SetSizer(root);

        send_button_->Bind(wxEVT_BUTTON, &MainFrame::OnSend, this);
        switch_discovery_->Bind(wxEVT_CHOICE, &MainFrame::OnDiscoveryChanged, this);
        import_icon_button_->Bind(wxEVT_BUTTON, &MainFrame::OnImportIcon, this);
        Bind(wxEVT_TIMER, &MainFrame::OnTimer, this, kLogTimerId);
        Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnClose, this);
        timer_.Start(100);
        UpdateDiscoveryControls();
        InitializeDefaultMetadataPreview();
        RefreshLog();
    }

private:
    void AddTextRow(wxWindow* parent, wxFlexGridSizer* form, const wxString& label,
                    wxTextCtrl*& control, const char* initial)
    {
        form->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
        control = new wxTextCtrl(parent, wxID_ANY, wxString::FromUTF8(initial));
        form->Add(control, 1, wxEXPAND);
        form->AddSpacer(1);
    }

    void AddFileRow(wxWindow* parent, wxFlexGridSizer* form, const wxString& label,
                    wxTextCtrl*& control, const char* initial)
    {
        form->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
        control = new wxTextCtrl(parent, wxID_ANY, wxString::FromUTF8(initial));
        form->Add(control, 1, wxEXPAND);
        wxButton* browse = new wxButton(parent, wxID_ANY, "Browse");
        form->Add(browse);
        browse->Bind(wxEVT_BUTTON, [this, control](wxCommandEvent&) {
            wxFileDialog dialog(this, "Select save file", "", control->GetValue(),
                                "All files (*.*)|*.*", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
            if (dialog.ShowModal() == wxID_OK)
            {
                control->SetValue(dialog.GetPath());
                if (control == file_b_)
                    LoadExtensionMetadata(control->GetValue());
            }
        });
    }

    void SetIconPreview(const std::vector<uint8_t>& png)
    {
        if (png.empty())
            return;

        wxMemoryInputStream stream(png.data(), png.size());
        wxImage image(stream, wxBITMAP_TYPE_PNG);
        if (!image.IsOk())
        {
            Log(state_, "could not decode icon PNG");
            return;
        }

        const int max_side = 64;
        const int width = image.GetWidth();
        const int height = image.GetHeight();
        const double scale = std::min(static_cast<double>(max_side) / width,
                                      static_cast<double>(max_side) / height);
        const int scaled_width = std::max(1, static_cast<int>(width * scale));
        const int scaled_height = std::max(1, static_cast<int>(height * scale));
        icon_bitmap_->SetBitmap(wxBitmap(image.Scale(scaled_width, scaled_height,
                                                     wxIMAGE_QUALITY_HIGH)));
        icon_bitmap_->GetParent()->Layout();
    }

    void LoadExtensionMetadata(const wxString& path)
    {
        imported_icon_path_.clear();
        imported_icon_label_->SetLabel("");
        ext_metadata_loaded_ = false;

        std::vector<uint8_t> extension;
        if (!ReadBinaryFile(ToUtf8(path), extension))
        {
            Log(state_, "could not open .ext file for metadata preview");
            return;
        }

        save_transfer::Metadata metadata;
        std::string error;
        if (!save_transfer::ParseExtensionFile(extension, metadata, &error))
        {
            Log(state_, "could not parse .ext metadata: " + error);
            return;
        }

        const std::string world_name = save_transfer::DecodeWorldNameUtf8(
            metadata.world_name_utf16be);
        world_name_->SetValue(wxString::FromUTF8(world_name.c_str()));

        std::ostringstream version;
        version << "transfer format 11, metadata version 2";
        auto version_tag = metadata.png_text_tags.find("4J_VERSION");
        if (version_tag != metadata.png_text_tags.end())
            version << ", 4J_VERSION " << version_tag->second;
        version_label_->SetLabel(wxString::FromUTF8(version.str().c_str()));

        std::ostringstream seed;
        seed << metadata.seed;
        seed_label_->SetLabel(wxString::FromUTF8(seed.str().c_str()));

        std::ostringstream options;
        options << "0x" << std::hex << metadata.host_options << " / 0x";
        auto extra_data_tag = metadata.png_text_tags.find("4J_EXTRADATA");
        if (extra_data_tag != metadata.png_text_tags.end())
            options << extra_data_tag->second;
        else
            options << "0";
        options_label_->SetLabel(wxString::FromUTF8(options.str().c_str()));

        std::vector<uint8_t> png;
        if (save_transfer::ExtractExtensionIconPng(extension, png, &error))
            SetIconPreview(png);

        ext_metadata_loaded_ = true;
        Log(state_, "loaded .ext metadata: " + world_name);
    }

    void InitializeDefaultMetadataPreview()
    {
        save_transfer::Metadata metadata;
        world_name_->Clear();
        version_label_->SetLabel("transfer format 11, metadata version 2 (default)");
        seed_label_->SetLabel("0");
        options_label_->SetLabel("0x3e9c / 0x79900a8");

        std::vector<uint8_t> extension;
        save_transfer::BuildDefaultExtensionFile(extension);
        std::vector<uint8_t> png;
        std::string error;
        if (save_transfer::ExtractExtensionIconPng(extension, png, &error))
            SetIconPreview(png);

        ext_metadata_loaded_ = true;
    }

    void OnImportIcon(wxCommandEvent&)
    {
        wxFileDialog dialog(this, "Import icon PNG", "", "",
                            "PNG files (*.png)|*.png|All files (*.*)|*.*",
                            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK)
            return;

        std::vector<uint8_t> png;
        const std::string path = ToUtf8(dialog.GetPath());
        if (!ReadBinaryFile(path, png))
        {
            Log(state_, "could not open imported icon PNG");
            return;
        }

        std::vector<uint8_t> temporary(save_transfer::kExtensionHeaderSize, 0);
        std::string error;
        if (!save_transfer::ReplaceExtensionIconPng(temporary, png, &error))
        {
            Log(state_, "could not import icon PNG: " + error);
            return;
        }

        imported_icon_path_ = path;
        imported_icon_label_->SetLabel(dialog.GetFilename());
        SetIconPreview(png);
        Log(state_, "imported icon PNG: " + path);
    }

    void UpdateDiscoveryControls()
    {
        const bool auto_detect = switch_discovery_->GetSelection() == 0;
        switch_ip_->Enable(!auto_detect);
    }

    void OnDiscoveryChanged(wxCommandEvent&)
    {
        UpdateDiscoveryControls();
    }

    void OnSend(wxCommandEvent&)
    {
        if (!state_.sender_running)
        {
            state_.auto_detect_switch = switch_discovery_->GetSelection() == 0;
            CopyToBuffer(state_.remote_host, sizeof(state_.remote_host),
                         switch_ip_->GetValue());
            CopyToBuffer(state_.send_path_a, sizeof(state_.send_path_a),
                         file_a_->GetValue());
            CopyToBuffer(state_.send_path_b, sizeof(state_.send_path_b),
                         file_b_->GetValue());
            state_.use_edited_world_name = ext_metadata_loaded_;
            state_.edited_world_name_utf8 = ToUtf8(world_name_->GetValue());
            state_.imported_icon_path = imported_icon_path_;
            StartSender(state_);
        }
        else
        {
            state_.sender_cancel = true;
        }
    }

    void OnTimer(wxTimerEvent&)
    {
        const bool running = state_.sender_running;
        send_button_->SetLabel(running ? "Cancel" : "Send");
        const TransferPhase phase = static_cast<TransferPhase>(state_.transfer_phase.load());
        wxString status_text = TransferPhaseLabel(phase);
        if (phase == TransferPhase::Error)
        {
            std::string detail;
            {
                std::lock_guard<std::mutex> lock(state_.mutex);
                detail = state_.transfer_status_detail;
            }
            if (!detail.empty())
                status_text += ": " + wxString::FromUTF8(detail.c_str());
        }
        status_->SetLabel(status_text);
        const uint64_t total = state_.total_bytes.load();
        const uint64_t sent = state_.bytes_sent.load();
        const int percent = total == 0 ? 0 : static_cast<int>(
            std::min<uint64_t>(100, (sent * 100) / total));
        progress_->SetValue(percent);
        RefreshLog();
    }

    wxString TransferPhaseLabel(TransferPhase phase) const
    {
        switch (phase)
        {
        case TransferPhase::Preparing:
            return "Preparing";
        case TransferPhase::Discovering:
            return "Discovering";
        case TransferPhase::Connecting:
            return "Connecting";
        case TransferPhase::WaitingForReceiver:
            return "Waiting for receiver";
        case TransferPhase::Sending:
            return "Sending";
        case TransferPhase::Completed:
            return "Completed";
        case TransferPhase::Error:
            return "Error";
        case TransferPhase::Cancelled:
            return "Cancelled";
        case TransferPhase::Idle:
        default:
            return "Ready";
        }
    }

    void RefreshLog()
    {
        std::vector<std::string> pending;
        {
            std::lock_guard<std::mutex> lock(state_.mutex);
            if (rendered_log_count_ > state_.log.size())
                rendered_log_count_ = 0;
            if (rendered_log_count_ == state_.log.size())
                return;

            pending.assign(state_.log.begin() + rendered_log_count_, state_.log.end());
            rendered_log_count_ = state_.log.size();
        }

        if (pending.empty())
            return;

        wxString text;
        for (const std::string& line : pending)
        {
            text += wxString::FromUTF8(line.c_str());
            text += '\n';
        }
        log_->AppendText(text);
        log_->ShowPosition(log_->GetLastPosition());
    }

    void OnClose(wxCloseEvent& event)
    {
        timer_.Stop();
        StopSender(state_);
        event.Skip();
    }

    AppState state_;
    wxChoice* switch_discovery_ = nullptr;
    wxTextCtrl* switch_ip_ = nullptr;
    wxTextCtrl* file_a_ = nullptr;
    wxTextCtrl* file_b_ = nullptr;
    wxTextCtrl* world_name_ = nullptr;
    wxStaticText* version_label_ = nullptr;
    wxStaticText* seed_label_ = nullptr;
    wxStaticText* options_label_ = nullptr;
    wxStaticBitmap* icon_bitmap_ = nullptr;
    wxButton* import_icon_button_ = nullptr;
    wxStaticText* imported_icon_label_ = nullptr;
    wxButton* send_button_ = nullptr;
    wxGauge* progress_ = nullptr;
    wxStaticText* status_ = nullptr;
    wxTextCtrl* log_ = nullptr;
    wxTimer timer_;
    size_t rendered_log_count_ = 0;
    bool ext_metadata_loaded_ = false;
    std::string imported_icon_path_;
};

class TransporterApp final : public wxApp
{
public:
    bool OnInit() override
    {
        wxInitAllImageHandlers();
        winsock_ = new WinsockScope();
        if (!winsock_->ok())
            return false;

        MainFrame* frame = new MainFrame();
        frame->Show(true);
        return true;
    }

    int OnExit() override
    {
        delete winsock_;
        winsock_ = nullptr;
        return wxApp::OnExit();
    }

private:
    WinsockScope* winsock_ = nullptr;
};
} // namespace

wxIMPLEMENT_APP(TransporterApp);
