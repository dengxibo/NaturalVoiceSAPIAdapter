#pragma once
#include <atlbase.h>
#include <sphelper.h>
#include <icu.h>
#include <functional>
#include <memory>
#include <cwctype>
#include <map>
#include <future>
#include <chrono>

// All positions are UTF-16 positions, like SAPI and the Windows HD word events.
struct HdPosition { ULONG source = ULONG_MAX; size_t flat = SIZE_MAX; };
struct HdSpan { const SPVTEXTFRAG* fragment; size_t begin, end; };
struct HdSentence { size_t begin, end; };
struct HdText {
    std::wstring flat;
    std::vector<HdPosition> positions;
    std::vector<HdSpan> spans;
    std::vector<HdSentence> sentences;
    explicit HdText(const SPVTEXTFRAG* fragments, const std::wstring& locale) {
        for (auto f = fragments; f; f = f->pNext) {
            size_t begin = flat.size();
            if (f->State.eAction == SPVA_Silence) {
                flat += wchar_t(0xfffc); positions.push_back({f->ulTextSrcOffset, begin});
            } else if (f->State.eAction == SPVA_Pronounce && f->State.pPhoneIds && !f->ulTextLen) {
                flat += wchar_t(0xfffc); positions.push_back({ULONG_MAX, begin});
            } else if (f->State.eAction != SPVA_Bookmark && f->State.eAction != SPVA_ParseUnknownTag && f->pTextStart) {
                for (ULONG i = 0; i < f->ulTextLen; ++i) {
                    positions.push_back({f->ulTextSrcOffset + i, flat.size()}); flat += f->pTextStart[i];
                }
            }
            spans.push_back({f, begin, flat.size()});
        }
        if (!flat.empty()) {
            UErrorCode error = U_ZERO_ERROR;
            std::string language = Utf8(locale);
            UBreakIterator* iterator = ubrk_open(UBRK_SENTENCE, language.c_str(), reinterpret_cast<const UChar*>(flat.data()), static_cast<int32_t>(flat.size()), &error);
            if (U_FAILURE(error) || !iterator) throw E_FAIL;
            int32_t begin = ubrk_first(iterator);
            for (int32_t end = ubrk_next(iterator); end != UBRK_DONE; begin = end, end = ubrk_next(iterator))
                sentences.push_back({static_cast<size_t>(begin), static_cast<size_t>(end)});
            ubrk_close(iterator);
        }
        if (sentences.empty()) sentences.push_back({0, flat.size()});
    }
};
struct HdSsml {
    std::wstring xml;
    std::vector<HdPosition> map;
    std::map<std::string, std::wstring> bookmarks;
    std::map<std::string, size_t> bookmarkPositions;
    std::vector<std::string> bookmarkOrder;
    size_t textStart = 0;
    void Tag(const std::wstring& tag) { xml += tag; map.resize(xml.size()); }
    void Text(const SPVTEXTFRAG* fragment, size_t offset, size_t length, size_t flatStart) {
        for (size_t i = 0; i < length; ++i) {
            auto escaped = Xml(std::wstring(1, fragment->pTextStart[offset+i]));
            xml += escaped;
            map.insert(map.end(), escaped.size(), {fragment->ulTextSrcOffset + static_cast<ULONG>(offset+i), flatStart+i});
        }
    }
    HdSsml(const HdText& text, size_t begin, size_t end, long mainRate, const std::wstring& locale, const std::wstring& name, bool forceUps) {
        textStart = begin;
        std::wstring synthesisLocale = locale;
        // The embedded parser selects the phoneme alphabet by the root locale;
        // unlike cloud SSML it rejects nested <lang> and phoneme xml:lang.
        for (const auto& span : text.spans) if (!forceUps && span.begin < end && span.end > begin && span.fragment->State.eAction == SPVA_Pronounce && span.fragment->State.LangID) {
            CComPtr<ISpPhoneConverter> converter; CheckHr(SpCreatePhoneConverter(span.fragment->State.LangID,nullptr,nullptr,&converter));
            CComPtr<ISpPhoneticAlphabetSelection> alphabet; BOOL ups=FALSE;
            if (SUCCEEDED(converter.QueryInterface(&alphabet))) CheckHr(alphabet->IsAlphabetUPS(&ups));
            if (ups) break;
            wchar_t language[LOCALE_NAME_MAX_LENGTH]{};
            Check(LCIDToLocaleName(span.fragment->State.LangID,language,LOCALE_NAME_MAX_LENGTH,0)); synthesisLocale = language; break;
        }
        Tag(L"<speak version=\"1.0\" xmlns=\"http://www.w3.org/2001/10/synthesis\" xml:lang=\"" + Xml(synthesisLocale) + L"\"><voice name=\"" + Xml(name) + L"\">");
        size_t marker = 0;
        for (const auto& span : text.spans) {
            const auto* f = span.fragment;
            if (f->State.eAction == SPVA_Bookmark) {
                if ((span.begin > begin && span.begin <= end) || (begin == 0 && span.begin == 0)) {
                    std::string id = "hd-" + std::to_string(marker++);
                    bookmarks[id] = f->pTextStart ? std::wstring(f->pTextStart, f->ulTextLen ? f->ulTextLen : wcslen(f->pTextStart)) : L"";
                    bookmarkPositions[id] = span.begin;
                    bookmarkOrder.push_back(id);
                    Tag(L"<bookmark mark=\"" + std::wstring(id.begin(), id.end()) + L"\"/>");
                }
                continue;
            }
            size_t from = (std::max)(span.begin, begin), to = (std::min)(span.end, end);
            if (from >= to) continue;
            if (f->State.eAction == SPVA_Silence) {
                Tag(L"<break time=\"" + std::to_wstring(f->State.SilenceMSecs) + L"ms\"/>"); continue;
            }
            long rate = std::clamp(mainRate + f->State.RateAdj, -10L, 10L);
            long percent = static_cast<long>(std::lround((std::pow(1.1, rate)-1)*100));
            auto signedValue = [](long value) { return std::wstring(value >= 0 ? L"+" : L"") + std::to_wstring(value) + L"%"; };
            Tag(L"<prosody rate=\"" + signedValue(percent) + L"\" pitch=\"" + signedValue(f->State.PitchAdj.MiddleAdj * 5) + L"\" volume=\"" + std::to_wstring((std::min)(f->State.Volume, 100UL)) + L"\">");
            bool emphasis = f->State.EmphAdj > 0;
            if (emphasis) Tag(L"<emphasis level=\"strong\">");
            std::wstring interpretation;
            if (f->State.eAction == SPVA_SpellOut) interpretation = L"characters";
            else if (f->State.Context.pCategory) {
                std::wstring category(f->State.Context.pCategory);
                if (category == L"number_cardinal") interpretation = L"cardinal";
                else if (category == L"number_ordinal") interpretation = L"ordinal";
                else if (category == L"phone_number") interpretation = L"telephone";
                else if (category == L"date") interpretation = L"date";
                else if (category == L"number_digit") interpretation = L"characters";
                else if (category == L"number_fraction") interpretation = L"fraction";
                else if (category == L"time") interpretation = L"time";
            }
            if (!interpretation.empty()) Tag(L"<say-as interpret-as=\"" + interpretation + L"\">");
            bool pronounce = f->State.eAction == SPVA_Pronounce && f->State.pPhoneIds;
            if (pronounce) {
                CComPtr<ISpPhoneConverter> converter;
                LANGID language = f->State.LangID ? f->State.LangID : LANGIDFROMLCID(LocaleNameToLCID(locale.c_str(),0));
                CheckHr(SpCreatePhoneConverter(forceUps ? 0 : language, nullptr, nullptr, &converter));
                wchar_t phones[SP_MAX_PRON_LENGTH * 8]{};
                CheckHr(converter->IdToPhone(f->State.pPhoneIds, phones));
                BOOL universal = FALSE; CComPtr<ISpPhoneticAlphabetSelection> alphabet;
                if (SUCCEEDED(converter.QueryInterface(&alphabet))) CheckHr(alphabet->IsAlphabetUPS(&universal));
                Tag(L"<phoneme alphabet=\"" + std::wstring(universal ? L"ups" : L"sapi") + L"\" ph=\"" + Xml(phones) + L"\">");
            }
            if (pronounce && !f->ulTextLen) Tag(L"x"); // Speak only the supplied phones, with no source-word event.
            else Text(f, from-span.begin, to-from, from);
            if (pronounce) Tag(L"</phoneme>");
            if (!interpretation.empty()) Tag(L"</say-as>");
            if (emphasis) Tag(L"</emphasis>");
            Tag(L"</prosody>");
        }
        Tag(L"</voice></speak>");
        if (xml.size() > 2 * 1024 * 1024) throw E_INVALIDARG;
    }
};
struct HdRawEvent { HdPipe::Type type; HdPipe::Event value; std::string text; };
struct HdCache {
    Handle file;
    uint64_t size = 0;
    bool silence = false;
    std::vector<HdRawEvent> events;
    HdCache() {
        wchar_t directory[MAX_PATH+1]{}, path[MAX_PATH+1]{};
        Check(GetTempPathW(MAX_PATH, directory)); Check(GetTempFileNameW(directory, L"hds", 0, path));
        file.value = CreateFileW(path, GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (file.value == INVALID_HANDLE_VALUE) { DWORD error = GetLastError(); DeleteFileW(path); throw HRESULT_FROM_WIN32(error); }
    }
    void Audio(const std::vector<BYTE>& data) {
        if (data.size() % 2) throw E_FAIL;
        DWORD written = 0; Check(WriteFile(file.value, data.data(), static_cast<DWORD>(data.size()), &written, nullptr));
        if (written != data.size()) throw E_FAIL; size += written;
    }
};
class HdSession {
    Handle writer, reader, stop, child, job;
    bool first = true;
    bool healthy = true;
    uint64_t requests = 0;
public:
    uint64_t traceCall = 0;
    bool Reusable() const { return healthy; }
    HdSession(const std::wstring& helper, const std::wstring& runtime, const std::wstring& model, const std::wstring& name) {
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
        Check(CreatePipe(&inRead, &inWrite, &security, 0)); Handle input(inRead); writer.value = inWrite;
        Check(CreatePipe(&outRead, &outWrite, &security, 0)); reader.value = outRead; Handle output(outWrite);
        Check(SetHandleInformation(writer.value, HANDLE_FLAG_INHERIT, 0)); Check(SetHandleInformation(reader.value, HANDLE_FLAG_INHERIT, 0));
        stop.value = CreateEventW(&security, TRUE, FALSE, nullptr);
        Handle errors(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));
        if (!stop.value || errors.value == INVALID_HANDLE_VALUE) throw HRESULT_FROM_WIN32(GetLastError());
        auto command = Quote(helper) + L" --sapi-events-v3 " + Quote(runtime) + L" " + Quote(model) + L" " + Quote(name) + L" " + std::to_wstring(reinterpret_cast<UINT_PTR>(stop.value));
        SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<BYTE> storage(size); auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        Check(InitializeProcThreadAttributeList(attributes, 1, 0, &size));
        HANDLE inherited[] = {input.value, output.value, stop.value, errors.value};
        if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) {
            DWORD error = GetLastError(); DeleteProcThreadAttributeList(attributes); throw HRESULT_FROM_WIN32(error);
        }
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input.value; startup.StartupInfo.hStdOutput = output.value; startup.StartupInfo.hStdError = errors.value; startup.lpAttributeList = attributes;
        PROCESS_INFORMATION process{};
        BOOL created = CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &process);
        DWORD error = GetLastError(); DeleteProcThreadAttributeList(attributes);
        if (!created) throw HRESULT_FROM_WIN32(error);
        child.value = process.hProcess; Handle thread(process.hThread); job.value = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) || !AssignProcessToJobObject(job.value, child.value)) {
            error = GetLastError(); TerminateProcess(child.value, 1); throw HRESULT_FROM_WIN32(error);
        }
        Check(ResumeThread(thread.value) != static_cast<DWORD>(-1));
    }
    ~HdSession() { writer.Reset(); if (stop.value) SetEvent(stop.value); if (child.value) WaitForSingleObject(child.value, 1500); }
    bool Request(const std::string& text, HdCache& cache, const std::function<bool()>& poll) {
        auto request=++requests; bool firstPcm=true;
        HdTrace::Log(traceCall,"synthesis_request",request,GetProcessId(child.value));
        auto send = [&](const void* data, size_t length) {
            auto bytes = static_cast<const BYTE*>(data);
            while (length) { DWORD written = 0; Check(WriteFile(writer.value, bytes, static_cast<DWORD>((std::min)(length,size_t(65536))), &written, nullptr)); if (!written) throw E_FAIL; bytes += written; length -= written; }
        };
        uint32_t length = static_cast<uint32_t>(text.size()); send(&length, sizeof(length)); send(text.data(), text.size());
        bool cancelledRequest=false;ULONGLONG cancelStarted=0;
        auto read = [&](void* target, DWORD count) {
            auto bytes = static_cast<BYTE*>(target);
            while (count) {
                if (!cancelledRequest&&!poll()) {cancelledRequest=true;cancelStarted=GetTickCount64();SetEvent(stop.value);HdTrace::Log(traceCall,"native_cancel_requested",request);}
                if(cancelledRequest&&GetTickCount64()-cancelStarted>2000){healthy=false;TerminateProcess(child.value,3);return false;}
                DWORD available = 0; Check(PeekNamedPipe(reader.value, nullptr, 0, nullptr, &available, nullptr));
                if (!available) { if (WaitForSingleObject(child.value, 10) == WAIT_OBJECT_0) throw E_FAIL; continue; }
                DWORD received = 0; Check(ReadFile(reader.value, bytes, (std::min)(available,count), &received, nullptr)); if (!received) throw E_FAIL;
                bytes += received; count -= received;
            }
            return true;
        };
        if (first) { uint32_t magic = 0; if (!read(&magic, sizeof(magic))) return false; if (magic != HdPipe::ReusableEventMagic) throw E_FAIL; first = false; }
        for (;;) {
            HdPipe::Header header{}; if (!read(&header, sizeof(header))) return false;
            if (header.size > HdPipe::MaximumFrame) throw E_FAIL;
            std::vector<BYTE> data(header.size); if (!read(data.data(), header.size)) return false;
            auto type = static_cast<HdPipe::Type>(header.type);
            if (type == HdPipe::Type::Complete) { uint32_t status = 1; if (data.size() != sizeof(status)) throw E_FAIL; memcpy(&status,data.data(),sizeof(status)); HdTrace::Log(traceCall,"synthesis_complete",request,cache.size); if(cancelledRequest&&(status==0||status==3)){HdTrace::Log(traceCall,"cancel_drained_reusable",request,GetProcessId(child.value));return false;} if (status) throw E_FAIL; return true; }
            if(cancelledRequest)continue;
            if (type == HdPipe::Type::Audio) { if(firstPcm) { HdTrace::Log(traceCall,"first_pcm",request,GetProcessId(child.value)); firstPcm=false; } cache.Audio(data); }
            else if (type >= HdPipe::Type::Word && type <= HdPipe::Type::Bookmark) {
                if (data.size() < sizeof(HdPipe::Event) || cache.events.size() >= 1000000) throw E_FAIL;
                HdRawEvent event{}; event.type = type; memcpy(&event.value,data.data(),sizeof(event.value));
                event.text.assign(reinterpret_cast<const char*>(data.data()+sizeof(event.value)),data.size()-sizeof(event.value)); cache.events.push_back(std::move(event));
            } else throw E_FAIL;
        }
    }
};
struct HdEvent { SPEVENT event{}; std::wstring bookmark; size_t flat = SIZE_MAX; };
static uint64_t HdBytes(uint64_t ticks) { return (ticks / 10000000 * 48000 + ticks % 10000000 * 48000 / 10000000) & ~uint64_t(1); }
static std::vector<HdEvent> HdEvents(const HdText& text, const HdSentence& sentence, const HdSsml& ssml, const HdCache& cache) {
    std::vector<HdEvent> result;
    std::map<std::string, bool> seenBookmarks;
    std::vector<HdRawEvent> visemes;
    uint64_t firstWord = UINT64_MAX;
    for (const auto& raw : cache.events) {
        HdEvent value;
        value.event.ullAudioStreamOffset = (std::min)(HdBytes(raw.value.ticks), cache.size);
        value.event.elParamType = SPET_LPARAM_IS_UNDEFINED;
        if (raw.type == HdPipe::Type::Word) {
            uint64_t end = uint64_t(raw.value.textOffset)+raw.value.textLength;
            if (end > ssml.map.size() || !raw.value.textLength) throw E_FAIL;
            ULONG begin = ULONG_MAX, last = 0;
            for (size_t i = raw.value.textOffset; i < end; ++i) if (ssml.map[i].source != ULONG_MAX) {
                if (begin == ULONG_MAX) { begin = ssml.map[i].source; value.flat = ssml.map[i].flat; }
                last = ssml.map[i].source;
            }
            if (begin == ULONG_MAX) continue;
            value.event.eEventId = SPEI_WORD_BOUNDARY; value.event.lParam = begin; value.event.wParam = last-begin+1;
            firstWord = (std::min)(firstWord, value.event.ullAudioStreamOffset);
        } else if (raw.type == HdPipe::Type::Bookmark) {
            auto found = ssml.bookmarks.find(raw.text);
            if (found == ssml.bookmarks.end() || seenBookmarks[raw.text]) continue;
            seenBookmarks[raw.text] = true; value.bookmark = found->second;
            value.event.eEventId = SPEI_TTS_BOOKMARK; value.event.elParamType = SPET_LPARAM_IS_STRING; value.event.wParam = _wtol(value.bookmark.c_str());
        } else if (raw.type == HdPipe::Type::Viseme) { visemes.push_back(raw); continue; }
        else continue; // HD currently returns word boundaries, not sentence boundaries.
        result.push_back(std::move(value));
    }
    size_t begin = sentence.begin, end = sentence.end;
    while (begin < end && (iswspace(text.flat[begin]) || text.flat[begin] == 0xfffc)) ++begin;
    while (end > begin && (iswspace(text.flat[end-1]) || text.flat[end-1] == 0xfffc)) --end;
    if (begin < end && firstWord != UINT64_MAX) {
        HdEvent event; event.event.eEventId = SPEI_SENTENCE_BOUNDARY; event.event.ullAudioStreamOffset = firstWord;
        event.event.lParam = text.positions[begin].source; event.event.wParam = text.positions[end-1].source-text.positions[begin].source+1; event.flat = begin;
        result.insert(result.begin(), std::move(event));
    }
    std::stable_sort(visemes.begin(),visemes.end(),[](const auto& a,const auto& b) { return a.value.ticks < b.value.ticks; });
    for (size_t i = 0; i < visemes.size(); ++i) {
        HdEvent event; event.event.eEventId = SPEI_VISEME; event.event.ullAudioStreamOffset = (std::min)(HdBytes(visemes[i].value.ticks),cache.size);
        uint64_t next = i+1 < visemes.size() ? (std::min)(HdBytes(visemes[i+1].value.ticks),cache.size) : cache.size;
        DWORD duration = static_cast<DWORD>((std::min)(uint64_t(65535),(next-event.event.ullAudioStreamOffset)/48));
        event.event.wParam = MAKEWPARAM(i+1 < visemes.size() ? visemes[i+1].value.value : 0,duration); event.event.lParam = MAKELPARAM(visemes[i].value.value,0); result.push_back(event);
    }
    // A bookmark-only request can legitimately produce no native audio/events.
    for (const auto& id : ssml.bookmarkOrder) if (!seenBookmarks[id]) {
        HdEvent event; event.bookmark = ssml.bookmarks.at(id); event.event.eEventId = SPEI_TTS_BOOKMARK; event.event.elParamType = SPET_LPARAM_IS_STRING;
        event.event.ullAudioStreamOffset = ssml.bookmarkPositions.at(id) <= ssml.textStart ? 0 : cache.size;
        event.event.wParam = _wtol(event.bookmark.c_str()); result.push_back(std::move(event));
    }
    std::stable_sort(result.begin(),result.end(),[](const auto& a,const auto& b) { return a.event.ullAudioStreamOffset < b.event.ullAudioStreamOffset; });
    return result;
}
struct HdPrepared {
    size_t index, begin, end;
    long rate;
    bool manual = false;
    bool expectsAudio = false;
    std::unique_ptr<HdSsml> ssml;
    HdCache cache;
    std::string request;
};
static std::unique_ptr<HdPrepared> HdPrepare(const HdText& text, size_t index, size_t begin, long rate, const std::wstring& locale, const std::wstring& name, bool forceUps) {
    auto work = std::make_unique<HdPrepared>();
    work->index=index; work->begin=begin; work->end=text.sentences[index].end; work->rate=rate;
    const SPVTEXTFRAG* pause=nullptr;
    for (const auto& span : text.spans) if (span.fragment->State.eAction==SPVA_Silence && span.begin>=begin && span.begin<work->end) {
        if (span.begin==begin) { pause=span.fragment; work->end=span.end; } else work->end=span.begin;
        break;
    }
    LANGID phoneticLanguage=0;
    for (const auto& span : text.spans) if (span.begin<work->end && span.end>begin && span.fragment->State.eAction==SPVA_Pronounce) {
        LANGID language=span.fragment->State.LangID ? span.fragment->State.LangID : LANGIDFROMLCID(LocaleNameToLCID(locale.c_str(),0));
        if (phoneticLanguage && language!=phoneticLanguage) { work->end=span.begin; break; }
        phoneticLanguage=language;
    }
    work->ssml=std::make_unique<HdSsml>(text,begin,work->end,rate,locale,name,forceUps);
    work->manual=pause || begin==work->end;
    work->expectsAudio=!work->manual && std::any_of(text.flat.begin()+begin,text.flat.begin()+work->end,[](wchar_t c) { return !iswspace(c) && !iswpunct(c); });
    if (pause) { work->cache.silence=true; work->cache.size=uint64_t(pause->State.SilenceMSecs)*48; }
    if (!work->manual) work->request=Utf8(work->ssml->xml);
    return work;
}
// The worker touches only the pipe and its private cache. SAPI site calls,
// COM phoneme conversion and all event delivery stay on the speaking thread.
class HdPending {
    std::unique_ptr<HdSession>& session;
    std::unique_ptr<HdPrepared> work;
    std::atomic<bool> cancelled{false};
    std::future<bool> future;
    bool Finish() {
        try {
            bool complete=future.get();
            if (!complete && session && !session->Reusable()) session.reset();
            return complete;
        } catch (...) { session.reset(); throw; }
    }
public:
    HdPending(std::unique_ptr<HdSession>& connection, std::unique_ptr<HdPrepared> next) : session(connection), work(std::move(next)) {
        if (!work->manual) future=std::async(std::launch::async,[this] { return session->Request(work->request,work->cache,[this] { return !cancelled.load(); }); });
    }
    bool Matches(size_t index,size_t begin,long rate) const { return work->index==index && work->begin==begin && work->rate==rate; }
    std::unique_ptr<HdPrepared> Take(const std::function<bool()>& poll) {
        bool interrupted=false;
        if (future.valid()) {
            while(future.wait_for(std::chrono::milliseconds(10))!=std::future_status::ready) if(!poll()) { cancelled=true; interrupted=true; break; }
            if(!Finish() || interrupted) return {};
        }
        return std::move(work);
    }
    ~HdPending() {
        if (future.valid()) { cancelled=true; try { Finish(); } catch (...) {} }
    }
};
static HRESULT HdSpeak(const SPVTEXTFRAG* fragments, ISpTTSEngineSite* site, const std::wstring& runtime, const std::wstring& model, const std::wstring& name, const std::wstring& helper, const std::wstring& locale, bool forceUps, std::unique_ptr<HdSession>& session,uint64_t traceCall) {
    HdText text(fragments, locale);
    long rate = 0; CheckHr(site->GetRate(&rate));
    ULONGLONG interests = 0; CheckHr(site->GetEventInterest(&interests));
    std::unique_ptr<HdPending> pending;
    enum class Action { None, Abort, Skip, Rate } action = Action::None;
    long skip = 0;
    auto poll = [&] {
        DWORD flags = site->GetActions();
        if (flags & SPVES_ABORT) { action = Action::Abort; return false; }
        if (flags & SPVES_SKIP) { SPVSKIPTYPE type{}; CheckHr(site->GetSkipInfo(&type,&skip)); if (type != SPVST_SENTENCE) { CheckHr(site->CompleteSkip(0)); } else { action = Action::Skip; return false; } }
        if (flags & SPVES_RATE) { long updated = 0; CheckHr(site->GetRate(&updated)); if (updated != rate) { rate = updated; action = Action::Rate; return false; } }
        return true;
    };
    size_t index = 0, resume = 0;
    bool sentenceAnnounced = false;
    uint64_t streamOffset = 0;
    while (index < text.sentences.size()) {
        action = Action::None;
        const auto sentence = text.sentences[index];
        size_t partBegin = (std::max)(sentence.begin,resume), partEnd = sentence.end;
        std::unique_ptr<HdPrepared> prepared;
        if (pending && pending->Matches(index,partBegin,rate)) { prepared=pending->Take(poll); pending.reset(); }
        else {
            pending.reset();
            prepared=HdPrepare(text,index,partBegin,rate,locale,name,forceUps);
            if (!prepared->manual) {
                if (!session) session=std::make_unique<HdSession>(helper,runtime,model,name);
                session->traceCall=traceCall;
                if (!session->Request(prepared->request,prepared->cache,poll)) { if(!session->Reusable())session.reset(); prepared.reset(); }
            }
        }
        // A native success status without PCM must not be accepted as a usable
        // prefetched sentence. Retry once in a fresh runtime, then report failure.
        if (prepared && prepared->expectsAudio && !prepared->cache.size) {
            session.reset(); prepared->cache.events.clear();
            session=std::make_unique<HdSession>(helper,runtime,model,name);
            session->traceCall=traceCall;
            if (!session->Request(prepared->request,prepared->cache,poll)) { if(!session->Reusable())session.reset(); prepared.reset(); }
            else if (!prepared->cache.size) throw E_FAIL;
        }
        size_t currentWord = (std::max)(sentence.begin,resume);
        if (prepared) {
            partEnd=prepared->end;
            auto& cache=prepared->cache;
            auto events = HdEvents(text,sentence,*prepared->ssml,cache);
            // Prepare at most one following part while the SAPI output queue
            // consumes this one. This bounds disk/cache use and CPU contention.
            size_t nextIndex=partEnd<sentence.end ? index : index+1;
            if (nextIndex<text.sentences.size()) {
                size_t nextBegin=nextIndex==index ? partEnd : text.sentences[nextIndex].begin;
                auto next=HdPrepare(text,nextIndex,nextBegin,rate,locale,name,forceUps);
                if (!next->manual && !session) session=std::make_unique<HdSession>(helper,runtime,model,name);
                if(session) session->traceCall=traceCall;
                pending=std::make_unique<HdPending>(session,std::move(next));
            }
            if (sentenceAnnounced) events.erase(std::remove_if(events.begin(),events.end(),[](const auto& event) { return event.event.eEventId == SPEI_SENTENCE_BOUNDARY; }),events.end());
            LARGE_INTEGER start{}; Check(SetFilePointerEx(cache.file.value,start,nullptr,FILE_BEGIN));
            size_t eventIndex = 0; uint64_t cursor = 0;
            for (;;) {
                if (!poll()) break;
                DWORD length = static_cast<DWORD>((std::min)(uint64_t(960),cache.size-cursor));
                while (eventIndex < events.size() && events[eventIndex].event.ullAudioStreamOffset <= cursor) {
                    auto& event = events[eventIndex++];
                    if (event.event.eEventId == SPEI_SENTENCE_BOUNDARY) sentenceAnnounced = true;
                    if (event.event.eEventId == SPEI_WORD_BOUNDARY) currentWord = event.flat;
                    if (interests & SPFEI(event.event.eEventId)) {
                        SPEVENT output = event.event; output.ullAudioStreamOffset += streamOffset;
                        if (output.elParamType == SPET_LPARAM_IS_STRING) output.lParam = reinterpret_cast<LPARAM>(event.bookmark.c_str());
                        CheckHr(site->AddEvents(&output,1));
                    }
                }
                if (!length) break;
                if (eventIndex < events.size() && events[eventIndex].event.ullAudioStreamOffset < cursor+length)
                    length = static_cast<DWORD>(events[eventIndex].event.ullAudioStreamOffset-cursor);
                std::vector<short> data(length/2);
                if (!cache.silence) { DWORD read = 0; Check(ReadFile(cache.file.value,data.data(),length,&read,nullptr)); if (read != length) throw E_FAIL; }
                USHORT volume = 100; CheckHr(site->GetVolume(&volume));
                if (volume < 100) for (auto& sample : data) sample = static_cast<short>(static_cast<int>(sample)*volume/100);
                ULONG offset = 0;
                while (offset < length) {
                    ULONG written = 0;
                    HdTrace::Log(traceCall,"write_begin",streamOffset+cursor,length-offset);
                    HRESULT writeResult=site->Write(reinterpret_cast<const BYTE*>(data.data())+offset,length-offset,&written);
                    HdTrace::Log(traceCall,"write_return",streamOffset+cursor,written);
                    if((FAILED(writeResult)||!written)&&(site->GetActions()&SPVES_ABORT)){action=Action::Abort;break;}
                    CheckHr(writeResult);
                    HdTrace::Pcm(traceCall,reinterpret_cast<const BYTE*>(data.data())+offset,written);
                    if (!written || written > length-offset || written%2) throw E_FAIL;
                    offset += written; cursor += written;
                }
                if(action==Action::Abort)break;
            }
            streamOffset += cursor;
            if (action == Action::None) {
                if (partEnd < sentence.end) { resume = partEnd; continue; }
                ++index; sentenceAnnounced = false; resume = index < text.sentences.size() ? text.sentences[index].begin : text.flat.size(); continue;
            }
        }
        pending.reset(); // Join/cancel obsolete prefetch before any new request.
        // A fully drained, idle synthesizer remains reusable after purging PCM.
        // Interrupted native requests already reset the connection above.
        if (action == Action::Abort) return S_OK;
        if (action == Action::Skip) {
            int64_t target = std::clamp(static_cast<int64_t>(index)+skip,int64_t(0),static_cast<int64_t>(text.sentences.size()));
            long actual = static_cast<long>(target-static_cast<int64_t>(index)); CheckHr(site->CompleteSkip(actual));
            if (actual != skip) return S_OK;
            index = static_cast<size_t>(target); resume = index < text.sentences.size() ? text.sentences[index].begin : text.flat.size();
            sentenceAnnounced = false;
        } else if (action == Action::Rate) resume = currentWord;
        else throw E_FAIL;
    }
    return S_OK;
}
