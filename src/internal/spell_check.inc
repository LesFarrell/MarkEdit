/* ---- Native Windows spell checking for Markdown prose. -----------------
   Scintilla stores UTF-8 byte positions while ISpellChecker reports UTF-16
   offsets, so each reported range is converted before applying an indicator.
   Lexer styles keep code, links, rules, and Markdown punctuation unmarked. */

static const GUID MARKEDIT_CLSID_SPELL_CHECKER_FACTORY =
    {0x7ab36653, 0x1796, 0x484b, {0xbd, 0xfa, 0xe7, 0x4f, 0x1d, 0xb7, 0xc1, 0xdc}};
static const GUID MARKEDIT_IID_SPELL_CHECKER_FACTORY =
    {0x8e018a9d, 0x2415, 0x4677, {0xbf, 0x08, 0x79, 0x4e, 0xa6, 0x1f, 0x94, 0xbb}};

static void copy_locale_name(WCHAR destination[LOCALE_NAME_MAX_LENGTH], const WCHAR *source) {
    wcsncpy(destination, source, LOCALE_NAME_MAX_LENGTH - 1);
    destination[LOCALE_NAME_MAX_LENGTH - 1] = 0;
}

static int create_spell_checker_for_language(const WCHAR *requested, int reportErrors) {
    ISpellCheckerFactory *factory = NULL;
    HRESULT hr = CoCreateInstance(&MARKEDIT_CLSID_SPELL_CHECKER_FACTORY, NULL,
        CLSCTX_INPROC_SERVER, &MARKEDIT_IID_SPELL_CHECKER_FACTORY, (void **)&factory);
    if (FAILED(hr) || !factory) return 0;

    WCHAR language[LOCALE_NAME_MAX_LENGTH] = L"";
    if (requested && requested[0]) {
        copy_locale_name(language, requested);
    } else if (!GetUserDefaultLocaleName(language, LOCALE_NAME_MAX_LENGTH))
        wcscpy(language, L"en-GB");

    BOOL supported = FALSE;
    ISpellCheckerFactory_IsSupported(factory, language, &supported);
    if (!supported) {
        IEnumString *languages = NULL;
        if (SUCCEEDED(ISpellCheckerFactory_get_SupportedLanguages(factory, &languages)) && languages) {
            LPOLESTR first = NULL;
            ULONG fetched = 0;
            if (IEnumString_Next(languages, 1, &first, &fetched) == S_OK && first) {
                copy_locale_name(language, first);
                CoTaskMemFree(first);
                supported = TRUE;
            }
            IEnumString_Release(languages);
        }
    }

    ISpellChecker *checker = NULL;
    if (supported && SUCCEEDED(ISpellCheckerFactory_CreateSpellChecker(
            factory, language, &checker)) && checker) {
        if (g_spellChecker) ISpellChecker_Release(g_spellChecker);
        g_spellChecker = checker;
        copy_locale_name(g_spellLanguage, language);
    }
    ISpellCheckerFactory_Release(factory);
    if (!checker && reportErrors)
        MessageBoxW(g_hMainWnd, L"The selected Windows spelling dictionary is unavailable.",
                    L"Spell Check", MB_OK | MB_ICONWARNING);
    return checker != NULL;
}

static void init_spell_checker(void) {
    create_spell_checker_for_language(g_spellLanguage, 0);
}

static void choose_spell_language(void) {
    ISpellCheckerFactory *factory = NULL;
    if (FAILED(CoCreateInstance(&MARKEDIT_CLSID_SPELL_CHECKER_FACTORY, NULL,
                                CLSCTX_INPROC_SERVER, &MARKEDIT_IID_SPELL_CHECKER_FACTORY,
                                (void **)&factory)) || !factory) return;
    IEnumString *languages = NULL;
    WCHAR **labels = NULL;
    int count = 0;
    if (SUCCEEDED(ISpellCheckerFactory_get_SupportedLanguages(factory, &languages)) && languages) {
        int capacity = 0;
        for (;;) {
            LPOLESTR language = NULL; ULONG fetched = 0;
            if (IEnumString_Next(languages, 1, &language, &fetched) != S_OK || !language) break;
            if (count >= capacity) {
                int next = capacity ? capacity * 2 : 16;
                WCHAR **grown = (WCHAR **)realloc(labels, (size_t)next * sizeof(WCHAR *));
                if (!grown) { CoTaskMemFree(language); break; }
                labels = grown; capacity = next;
            }
            WCHAR displayName[LOCALE_NAME_MAX_LENGTH] = L"";
            GetLocaleInfoEx(language, LOCALE_SLOCALIZEDDISPLAYNAME, displayName,
                            LOCALE_NAME_MAX_LENGTH);
            size_t labelCapacity = wcslen(language) + wcslen(displayName) + 8;
            labels[count] = (WCHAR *)malloc(labelCapacity * sizeof(WCHAR));
            if (labels[count]) {
                if (displayName[0]) swprintf(labels[count], labelCapacity, L"%ls — %ls", displayName, language);
                else wcscpy(labels[count], language);
                count++;
            }
            CoTaskMemFree(language);
        }
        IEnumString_Release(languages);
    }
    ISpellCheckerFactory_Release(factory);
    if (!count) {
        MessageBoxW(g_hMainWnd, L"Windows has no installed spelling dictionaries.",
                    L"Spell Check", MB_OK | MB_ICONINFORMATION);
        free(labels); return;
    }
    int selected = show_choice_dialog(L"Spell Check Language",
                                      L"Choose an installed Windows dictionary:", labels, count, L"Open");
    if (selected >= 0 && selected < count) {
        WCHAR locale[LOCALE_NAME_MAX_LENGTH];
        WCHAR *dash = wcsrchr(labels[selected], L'—');
        const WCHAR *source = dash ? dash + 1 : labels[selected];
        while (*source == L' ') source++;
        copy_locale_name(locale, source);
        if (create_spell_checker_for_language(locale, 1)) {
            g_spellCheckEnabled = 1;
            save_layout_settings(); /* persists the language and the enabled state */
            schedule_spell_check();
            refresh_menu(g_hMainWnd);
        }
    }
    for (int i = 0; i < count; i++) free(labels[i]);
    free(labels);
}

static int markdown_style_is_prose(int style) {
    return style == SCE_MARKDOWN_DEFAULT ||
           style == SCE_MARKDOWN_STRONG1 || style == SCE_MARKDOWN_STRONG2 ||
           style == SCE_MARKDOWN_EM1 || style == SCE_MARKDOWN_EM2 ||
           (style >= SCE_MARKDOWN_HEADER1 && style <= SCE_MARKDOWN_HEADER6) ||
           style == SCE_MARKDOWN_BLOCKQUOTE || style == SCE_MARKDOWN_STRIKEOUT;
}

static void schedule_spell_check(void) {
    if (!g_hMainWnd) return;
    if (g_largeFile) {
        KillTimer(g_hMainWnd, IDT_SPELL_CHECK);
        return;
    }
    SetTimer(g_hMainWnd, IDT_SPELL_CHECK, SPELL_CHECK_DELAY_MS, NULL);
}

/* ISpellChecker_Check() below is a single synchronous COM call over the
   *entire* document (there's no incremental/ranged spellcheck API), run
   right on the UI thread with no chunking or background thread. That's
   unnoticeable at normal typing volume, since each keystroke rechecks a
   document that's already close to this size - but pasting a large block
   makes the document balloon in one step, and the native spellchecker is
   slow per byte, so the very next check after a big paste can block the UI
   thread (and the whole app's message pump) for a long, visible stretch.
   Skipping the check above this size trades spelling squiggles in huge
   documents for not freezing on every large paste. */
#define SPELLCHECK_MAX_LENGTH (256 * 1024)

static void perform_spell_check(void) {
    if (!g_hEdit) return;
    if (g_hMainWnd) KillTimer(g_hMainWnd, IDT_SPELL_CHECK);
    if (g_largeFile) return;

    Sci_Position textLength = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
    SendMessageA(g_hEdit, SCI_SETINDICATORCURRENT, INDICATOR_SPELLING, 0);
    SendMessageA(g_hEdit, SCI_INDICATORCLEARRANGE, 0, (LPARAM)textLength);
    if (!g_spellCheckEnabled || !g_spellChecker ||
        (g_activeLanguage != LANG_MARKDOWN && g_activeLanguage != LANG_TEXT) ||
        textLength <= 0 || textLength > SPELLCHECK_MAX_LENGTH) return;

    /* Ensure the Markdown styles used to exclude code and links are current. */
    SendMessageA(g_hEdit, SCI_COLOURISE, 0, (LPARAM)-1);
    Sci_Position ignoredLength = 0;
    char *utf8 = get_edit_text(g_hEdit, &ignoredLength);
    if (!utf8) return;

    int wideLength = 0;
    WCHAR *wide = to_wide_n(utf8, (size_t)textLength, &wideLength);
    if (!wide || wideLength <= 0) {
        free(wide);
        free(utf8);
        return;
    }

    IEnumSpellingError *errors = NULL;
    if (SUCCEEDED(ISpellChecker_Check(g_spellChecker, wide, &errors)) && errors) {
        for (;;) {
            ISpellingError *error = NULL;
            if (IEnumSpellingError_Next(errors, &error) != S_OK || !error) break;
            ULONG wideStart = 0, wideSpan = 0;
            CORRECTIVE_ACTION action = CORRECTIVE_ACTION_NONE;
            ISpellingError_get_StartIndex(error, &wideStart);
            ISpellingError_get_Length(error, &wideSpan);
            ISpellingError_get_CorrectiveAction(error, &action);
            if (action != CORRECTIVE_ACTION_NONE && wideSpan > 0 &&
                wideStart <= (ULONG)wideLength && wideSpan <= (ULONG)wideLength - wideStart) {
                int byteStart = WideCharToMultiByte(CP_UTF8, 0, wide, (int)wideStart,
                    NULL, 0, NULL, NULL);
                int byteSpan = WideCharToMultiByte(CP_UTF8, 0, wide + wideStart,
                    (int)wideSpan, NULL, 0, NULL, NULL);
                /* Plain text has no lexer at all (see the LANG_TEXT entry in
                   LANGUAGES) to distinguish code/links from prose the way
                   Markdown's does, so every character in it is spellable. */
                int spellable = g_activeLanguage == LANG_TEXT || markdown_style_is_prose(
                    (int)SendMessageA(g_hEdit, SCI_GETSTYLEAT, (WPARAM)byteStart, 0));
                if (byteSpan > 0 && byteStart >= 0 && byteStart + byteSpan <= textLength &&
                    spellable) {
                    SendMessageA(g_hEdit, SCI_SETINDICATORVALUE, 1, 0);
                    SendMessageA(g_hEdit, SCI_INDICATORFILLRANGE,
                                 (WPARAM)byteStart, (LPARAM)byteSpan);
                }
            }
            ISpellingError_Release(error);
        }
        IEnumSpellingError_Release(errors);
    }
    free(wide);
    free(utf8);
}

/* ---- spell.check for plugins. ------------------------------------------
   A check costs about 0.1 ms for every word the dictionary rejects, nearly
   all of it spent enumerating the errors, and searches such as anagram
   solving reject almost every word they try. So batches are split between
   worker threads, each in the multithreaded apartment with a checker of its
   own: that runs about three times faster than g_spellChecker does in the
   UI thread's apartment, and scales with the number of processors. Words
   added to the dictionary count; the session's Ignore Word list doesn't. */

#define PLUGIN_SPELL_MAX_THREADS 8
#define PLUGIN_SPELL_WORDS_PER_THREAD 1000

typedef struct {
    const WCHAR *language;
    const WCHAR *text;  /* the slice's words, separated by newlines */
    const int *starts;  /* where each word begins in text */
    unsigned char *misspelled;
    int count;
    int checked;
} PluginSpellSlice;

static int plugin_spell_word_at(const PluginSpellSlice *slice, ULONG offset) {
    int low = 0, high = slice->count - 1;
    while (low < high) {
        int middle = (low + high + 1) / 2;
        if ((ULONG)slice->starts[middle] <= offset) low = middle;
        else high = middle - 1;
    }
    return low;
}

static unsigned __stdcall plugin_spell_worker(void *argument) {
    PluginSpellSlice *slice = (PluginSpellSlice *)argument;
    if (!slice->text[0]) { slice->checked = 1; return 0; }
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) return 0;
    ISpellCheckerFactory *factory = NULL;
    ISpellChecker *checker = NULL;
    IEnumSpellingError *errors = NULL;
    if (SUCCEEDED(CoCreateInstance(&MARKEDIT_CLSID_SPELL_CHECKER_FACTORY, NULL,
            CLSCTX_INPROC_SERVER, &MARKEDIT_IID_SPELL_CHECKER_FACTORY, (void **)&factory)) &&
        factory && SUCCEEDED(ISpellCheckerFactory_CreateSpellChecker(
            factory, slice->language, &checker)) &&
        checker && SUCCEEDED(ISpellChecker_Check(checker, slice->text, &errors)) && errors) {
        slice->checked = 1;
        for (;;) {
            ISpellingError *error = NULL;
            if (IEnumSpellingError_Next(errors, &error) != S_OK || !error) break;
            ULONG start = 0;
            CORRECTIVE_ACTION action = CORRECTIVE_ACTION_NONE;
            ISpellingError_get_StartIndex(error, &start);
            ISpellingError_get_CorrectiveAction(error, &action);
            /* Deleting is the advice for a repeated word, which is spelled
               correctly; a list of words often repeats one. */
            if (action != CORRECTIVE_ACTION_NONE && action != CORRECTIVE_ACTION_DELETE)
                slice->misspelled[plugin_spell_word_at(slice, start)] = 1;
            ISpellingError_Release(error);
        }
    }
    if (errors) IEnumSpellingError_Release(errors);
    if (checker) ISpellChecker_Release(checker);
    if (factory) ISpellCheckerFactory_Release(factory);
    CoUninitialize();
    return 0;
}

static char *plugin_spell_language(void) {
    return g_spellChecker ? to_utf8(g_spellLanguage) : NULL;
}

static int plugin_spell_check(const char *const *words, int count, unsigned char *misspelled) {
    if (!g_spellChecker) return 0;
    int threads = (count + PLUGIN_SPELL_WORDS_PER_THREAD - 1) / PLUGIN_SPELL_WORDS_PER_THREAD;
    int processors = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (threads > processors) threads = processors;
    if (threads > PLUGIN_SPELL_MAX_THREADS) threads = PLUGIN_SPELL_MAX_THREADS;
    if (threads < 1) threads = 1;

    /* Words that aren't valid UTF-8 are misspelled, and take no room. */
    int *starts = (int *)malloc((size_t)count * sizeof(int));
    if (!starts) return 0;
    size_t total = 0;
    for (int i = 0; i < count; i++) {
        int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, words[i], -1, NULL, 0) - 1;
        misspelled[i] = length < 0;
        starts[i] = length < 0 ? 0 : length;
        total += (size_t)starts[i] + 1;
    }
    WCHAR *text = (WCHAR *)malloc(total * sizeof(WCHAR));
    if (!text) { free(starts); return 0; }

    /* Each slice's words are separated by newlines and its last one ends
       with a NUL, so the slices share one buffer. */
    PluginSpellSlice slices[PLUGIN_SPELL_MAX_THREADS] = {0};
    int perSlice = (count + threads - 1) / threads;
    size_t position = 0;
    for (int s = 0; s < threads; s++) {
        int first = s * perSlice;
        int last = first + perSlice < count ? first + perSlice : count;
        slices[s] = (PluginSpellSlice){ g_spellLanguage, text + position, starts + first,
                                        misspelled + first, last - first, 0 };
        size_t sliceStart = position;
        for (int i = first; i < last; i++) {
            int length = starts[i];
            starts[i] = (int)(position - sliceStart);
            if (length) MultiByteToWideChar(CP_UTF8, 0, words[i], -1, text + position, length + 1);
            position += (size_t)length;
            text[position++] = i == last - 1 ? 0 : L'\n';
        }
    }

    HANDLE handles[PLUGIN_SPELL_MAX_THREADS];
    int started = 0;
    while (started < threads) {
        handles[started] = (HANDLE)_beginthreadex(NULL, 0, plugin_spell_worker,
                                                  &slices[started], 0, NULL);
        if (!handles[started]) break;
        started++;
    }
    if (started) WaitForMultipleObjects((DWORD)started, handles, TRUE, INFINITE);
    int checked = started == threads;
    for (int s = 0; s < started; s++) {
        checked = checked && slices[s].checked;
        CloseHandle(handles[s]);
    }
    free(text);
    free(starts);
    return checked;
}
