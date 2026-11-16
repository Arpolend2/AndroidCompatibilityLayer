// Overlay of core/framework/gen-src/android/text/BidiFormatter.java (tools/build-framework.sh).
// It differs in being one: the generated class gave null for every unicodeWrap, markBefore and markAfter, and Deezer's
// search stopped at its first result (an R8 null check of what unicodeWrap gave: "Attempt to invoke virtual method
// getClass() on a null object reference", 0.1.0.699). Here it is Android 9's BidiFormatter: the text wrapped in an
// embedding where its direction is not the context's, with a mark after it (and before it, with stereo reset) where its
// edges would run into the text around it. The directions are estimated here (firstStrong, entryDir, exitDir) as Android's
// TextDirectionHeuristics.FIRSTSTRONG_LTR and DirectionalityEstimator do, since Megler's TextDirectionHeuristics are one
// object for LTR, FIRSTSTRONG_LTR, ANYRTL_LTR and LOCALE, which says "not right to left" whatever the text: that object
// given is taken for FIRSTSTRONG_LTR, the default (an explicit LTR is rare), and RTL is right to left.
package android.text;

import java.util.Locale;

public final class BidiFormatter {
    private static final char LRE = '‪', RLE = '‫', PDF = '‬';
    private static final String LRM_STRING = "‎", RLM_STRING = "‏", EMPTY_STRING = "";
    private static final int DIR_LTR = -1, DIR_UNKNOWN = 0, DIR_RTL = 1;

    private final boolean mIsRtlContext;
    private final boolean mStereoReset;
    private final TextDirectionHeuristic mDefaultTextDirectionHeuristic;

    private BidiFormatter(boolean isRtlContext, boolean stereoReset, TextDirectionHeuristic heuristic) {
        mIsRtlContext = isRtlContext;
        mStereoReset = stereoReset;
        mDefaultTextDirectionHeuristic = heuristic;
    }

    public static BidiFormatter getInstance() { return new Builder().build(); }
    public static BidiFormatter getInstance(boolean rtlContext) { return new Builder(rtlContext).build(); }
    public static BidiFormatter getInstance(Locale locale) { return new Builder(locale).build(); }

    public boolean isRtlContext() { return mIsRtlContext; }
    public boolean getStereoReset() { return mStereoReset; }

    public String markAfter(CharSequence str, TextDirectionHeuristic heuristic) { return markAfter(str, rtl(str, heuristic)); }
    public String markBefore(CharSequence str, TextDirectionHeuristic heuristic) { return markBefore(str, rtl(str, heuristic)); }

    private String markAfter(CharSequence str, boolean isRtl) {
        if (!mIsRtlContext && (isRtl || exitDir(str) == DIR_RTL)) return LRM_STRING;
        if (mIsRtlContext && (!isRtl || exitDir(str) == DIR_LTR)) return RLM_STRING;
        return EMPTY_STRING;
    }
    private String markBefore(CharSequence str, boolean isRtl) {
        if (!mIsRtlContext && (isRtl || entryDir(str) == DIR_RTL)) return LRM_STRING;
        if (mIsRtlContext && (!isRtl || entryDir(str) == DIR_LTR)) return RLM_STRING;
        return EMPTY_STRING;
    }

    public boolean isRtl(String str) { return isRtl((CharSequence) str); }
    public boolean isRtl(CharSequence str) { return rtl(str, mDefaultTextDirectionHeuristic); }

    public CharSequence unicodeWrap(CharSequence str, TextDirectionHeuristic heuristic, boolean isolate) {
        if (str == null) return null;
        final boolean isRtl = rtl(str, heuristic);
        SpannableStringBuilder result = new SpannableStringBuilder();
        if (getStereoReset() && isolate) result.append(markBefore(str, isRtl));
        if (isRtl != mIsRtlContext) {
            result.append(isRtl ? RLE : LRE);
            result.append(str);
            result.append(PDF);
        } else {
            result.append(str);
        }
        if (isolate) result.append(markAfter(str, isRtl));
        return result;
    }
    public CharSequence unicodeWrap(CharSequence str, TextDirectionHeuristic heuristic) { return unicodeWrap(str, heuristic, true); }
    public CharSequence unicodeWrap(CharSequence str, boolean isolate) { return unicodeWrap(str, mDefaultTextDirectionHeuristic, isolate); }
    public CharSequence unicodeWrap(CharSequence str) { return unicodeWrap(str, mDefaultTextDirectionHeuristic, true); }

    public String unicodeWrap(String str, TextDirectionHeuristic heuristic, boolean isolate) {
        if (str == null) return null;
        return unicodeWrap((CharSequence) str, heuristic, isolate).toString();
    }
    public String unicodeWrap(String str, TextDirectionHeuristic heuristic) { return unicodeWrap(str, heuristic, true); }
    public String unicodeWrap(String str, boolean isolate) { return unicodeWrap(str, mDefaultTextDirectionHeuristic, isolate); }
    public String unicodeWrap(String str) { return unicodeWrap(str, mDefaultTextDirectionHeuristic, true); }

    /** Whether str is right to left by heuristic: Megler's LTR object as FIRSTSTRONG_LTR, RTL as RTL, another as it says. */
    private static boolean rtl(CharSequence str, TextDirectionHeuristic heuristic) {
        if (heuristic == null || heuristic == TextDirectionHeuristics.FIRSTSTRONG_LTR) return firstStrong(str) == DIR_RTL;
        if (heuristic == TextDirectionHeuristics.RTL) return true;
        return heuristic.isRtl(str, 0, str.length());
    }

    /** The direction of str's first strong character outside isolates (LRI, RLI, FSI ... PDI), as FIRSTSTRONG_LTR. */
    private static int firstStrong(CharSequence str) {
        int isolates = 0;
        for (int i = 0, n = str.length(); i < n; ) {
            int cp = Character.codePointAt(str, i);
            i += Character.charCount(cp);
            if (cp == 0x2066 || cp == 0x2067 || cp == 0x2068) { isolates++; continue; }
            if (cp == 0x2069) { if (isolates > 0) isolates--; continue; }
            if (isolates > 0) continue;
            switch (Character.getDirectionality(cp)) {
                case Character.DIRECTIONALITY_LEFT_TO_RIGHT: return DIR_LTR;
                case Character.DIRECTIONALITY_RIGHT_TO_LEFT:
                case Character.DIRECTIONALITY_RIGHT_TO_LEFT_ARABIC: return DIR_RTL;
                default: break;
            }
        }
        return DIR_UNKNOWN;
    }

    /** The direction str begins with: its first strong character's, or that of the embedding it is in (Android's getEntryDir). */
    private static int entryDir(CharSequence str) {
        int i = 0, n = str.length();
        int embeddingLevel = 0, embeddingLevelDir = DIR_UNKNOWN, firstNonEmptyEmbeddingLevel = 0;
        while (i < n && firstNonEmptyEmbeddingLevel == 0) {
            int cp = Character.codePointAt(str, i);
            i += Character.charCount(cp);
            switch (DirectionalityEstimator.getDirectionality(cp)) {
                case Character.DIRECTIONALITY_LEFT_TO_RIGHT_EMBEDDING:
                case Character.DIRECTIONALITY_LEFT_TO_RIGHT_OVERRIDE: ++embeddingLevel; embeddingLevelDir = DIR_LTR; break;
                case Character.DIRECTIONALITY_RIGHT_TO_LEFT_EMBEDDING:
                case Character.DIRECTIONALITY_RIGHT_TO_LEFT_OVERRIDE: ++embeddingLevel; embeddingLevelDir = DIR_RTL; break;
                case Character.DIRECTIONALITY_POP_DIRECTIONAL_FORMAT: --embeddingLevel; embeddingLevelDir = DIR_UNKNOWN; break;
                case Character.DIRECTIONALITY_BOUNDARY_NEUTRAL: break;
                case Character.DIRECTIONALITY_LEFT_TO_RIGHT:
                    if (embeddingLevel == 0) return DIR_LTR;
                    firstNonEmptyEmbeddingLevel = embeddingLevel;
                    break;
                case Character.DIRECTIONALITY_RIGHT_TO_LEFT:
                case Character.DIRECTIONALITY_RIGHT_TO_LEFT_ARABIC:
                    if (embeddingLevel == 0) return DIR_RTL;
                    firstNonEmptyEmbeddingLevel = embeddingLevel;
                    break;
                default: firstNonEmptyEmbeddingLevel = embeddingLevel; break;
            }
        }
        if (firstNonEmptyEmbeddingLevel == 0) return DIR_UNKNOWN;
        if (embeddingLevelDir != DIR_UNKNOWN) return embeddingLevelDir;
        // The embedding that opened that level, looked for backwards from where the scan stopped.
        while (i > 0) {
            int cp = Character.codePointBefore(str, i);
            i -= Character.charCount(cp);
            switch (DirectionalityEstimator.getDirectionality(cp)) {
                case Character.DIRECTIONALITY_LEFT_TO_RIGHT_EMBEDDING:
                case Character.DIRECTIONALITY_LEFT_TO_RIGHT_OVERRIDE:
                    if (firstNonEmptyEmbeddingLevel == embeddingLevel) return DIR_LTR;
                    --embeddingLevel;
                    break;
                case Character.DIRECTIONALITY_RIGHT_TO_LEFT_EMBEDDING:
                case Character.DIRECTIONALITY_RIGHT_TO_LEFT_OVERRIDE:
                    if (firstNonEmptyEmbeddingLevel == embeddingLevel) return DIR_RTL;
                    --embeddingLevel;
                    break;
                case Character.DIRECTIONALITY_POP_DIRECTIONAL_FORMAT: ++embeddingLevel; break;
                default: break;
            }
        }
        return DIR_UNKNOWN;
    }

    /** The direction str ends with: its last strong character's, or that of the embedding it is in (Android's getExitDir). */
    private static int exitDir(CharSequence str) {
        int i = str.length();
        int embeddingLevel = 0, lastNonEmptyEmbeddingLevel = 0;
        while (i > 0) {
            int cp = Character.codePointBefore(str, i);
            i -= Character.charCount(cp);
            switch (DirectionalityEstimator.getDirectionality(cp)) {
                case Character.DIRECTIONALITY_LEFT_TO_RIGHT:
                    if (embeddingLevel == 0) return DIR_LTR;
                    if (lastNonEmptyEmbeddingLevel == 0) lastNonEmptyEmbeddingLevel = embeddingLevel;
                    break;
                case Character.DIRECTIONALITY_LEFT_TO_RIGHT_EMBEDDING:
                case Character.DIRECTIONALITY_LEFT_TO_RIGHT_OVERRIDE:
                    if (lastNonEmptyEmbeddingLevel == embeddingLevel) return DIR_LTR;
                    --embeddingLevel;
                    break;
                case Character.DIRECTIONALITY_RIGHT_TO_LEFT:
                case Character.DIRECTIONALITY_RIGHT_TO_LEFT_ARABIC:
                    if (embeddingLevel == 0) return DIR_RTL;
                    if (lastNonEmptyEmbeddingLevel == 0) lastNonEmptyEmbeddingLevel = embeddingLevel;
                    break;
                case Character.DIRECTIONALITY_RIGHT_TO_LEFT_EMBEDDING:
                case Character.DIRECTIONALITY_RIGHT_TO_LEFT_OVERRIDE:
                    if (lastNonEmptyEmbeddingLevel == embeddingLevel) return DIR_RTL;
                    --embeddingLevel;
                    break;
                case Character.DIRECTIONALITY_POP_DIRECTIONAL_FORMAT: ++embeddingLevel; break;
                case Character.DIRECTIONALITY_BOUNDARY_NEUTRAL: break;
                default:
                    if (lastNonEmptyEmbeddingLevel == 0) lastNonEmptyEmbeddingLevel = embeddingLevel;
                    break;
            }
        }
        return DIR_UNKNOWN;
    }

    /** Whether the locale's script is written right to left: its language's, as ICU's likely subtags would say for these. */
    private static boolean rtlLocale(Locale locale) {
        if (locale == null) return false;
        String script = locale.getScript();
        if (script != null && !script.isEmpty()) return script.equals("Arab") || script.equals("Hebr") || script.equals("Thaa")
            || script.equals("Syrc") || script.equals("Nkoo") || script.equals("Adlm") || script.equals("Rohg");
        switch (locale.getLanguage()) {
            case "ar": case "fa": case "he": case "iw": case "ur": case "yi": case "ji": case "ps": case "sd": case "ug":
            case "ckb": case "dv": case "syr": case "ks": case "mzn": case "glk": case "lrc":
                return true;
            default:
                return false;
        }
    }

    public static final class Builder {
        private boolean mIsRtlContext;
        private boolean mStereoReset = true;
        private TextDirectionHeuristic mTextDirectionHeuristic;

        public Builder() { this(Locale.getDefault()); }
        public Builder(boolean rtlContext) { mIsRtlContext = rtlContext; }
        public Builder(Locale locale) { mIsRtlContext = rtlLocale(locale); }

        public Builder stereoReset(boolean stereoReset) { mStereoReset = stereoReset; return this; }
        public Builder setTextDirectionHeuristic(TextDirectionHeuristic heuristic) { mTextDirectionHeuristic = heuristic; return this; }
        public BidiFormatter build() {
            return new BidiFormatter(mIsRtlContext, mStereoReset,
                mTextDirectionHeuristic != null ? mTextDirectionHeuristic : TextDirectionHeuristics.FIRSTSTRONG_LTR);
        }
    }

    public static class DirectionalityEstimator {
        protected DirectionalityEstimator() {}
        public static byte getDirectionality(int codePoint) { return Character.getDirectionality(codePoint); }
    }
}
