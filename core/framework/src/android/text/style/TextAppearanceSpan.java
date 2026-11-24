package android.text.style;

import android.content.Context;
import android.content.res.ColorStateList;
import android.content.res.TypedArray;
import android.graphics.Typeface;
import android.os.LocaleList;
import android.os.Parcel;
import android.text.ParcelableSpan;
import android.text.TextUtils;

public class TextAppearanceSpan extends MetricAffectingSpan implements ParcelableSpan {
    private final String mFamily; private final int mStyle, mSize; private final ColorStateList mColor, mLinkColor;
    private int mFontWeight = -1, mShadowColor;
    private LocaleList mLocales;
    private float mShadowRadius, mShadowDx, mShadowDy, mLetterSpacing;
    private boolean mHasElegantTextHeight, mElegantTextHeight, mHasLetterSpacing;
    private String mFontFeatures, mFontVariation;
    public TextAppearanceSpan(Context c, int appearance) { this(c, appearance, -1); }
    public TextAppearanceSpan(Context c, int appearance, int colorList) {
        TypedArray a = c.obtainStyledAttributes(appearance, megler.S.TextAppearance);
        ColorStateList col = a.getColorStateList(megler.S.TextAppearance_textColor);
        mLinkColor = a.getColorStateList(megler.S.TextAppearance_textColorLink);
        mSize = a.getDimensionPixelSize(megler.S.TextAppearance_textSize, -1);
        mStyle = a.getInt(megler.S.TextAppearance_textStyle, 0);
        String fam = a.getString(megler.S.TextAppearance_fontFamily);
        if (fam == null) { int tf = a.getInt(megler.S.TextAppearance_typeface, 0); fam = tf == 1 ? "sans" : tf == 2 ? "serif" : tf == 3 ? "monospace" : null; }
        mFamily = fam;
        a.recycle();
        mColor = col;
    }
    public TextAppearanceSpan(String family, int style, int size, ColorStateList color, ColorStateList linkColor) { mFamily = family; mStyle = style; mSize = size; mColor = color; mLinkColor = linkColor; }
    public TextAppearanceSpan(Parcel source) {
        mFamily = source.readString();
        mStyle = source.readInt();
        mSize = source.readInt();
        mColor = source.readInt() != 0 ? ColorStateList.CREATOR.createFromParcel(source) : null;
        mLinkColor = source.readInt() != 0 ? ColorStateList.CREATOR.createFromParcel(source) : null;
        int pid = source.readInt(), typefaceId = source.readInt();
        if (pid >= 0 && typefaceId >= 0 && pid == android.os.Process.myPid())
            throw new UnsupportedOperationException("Parcel contains an unsupported local typeface");
        mFontWeight = source.readInt();
        String localesClass = source.readString();
        if (localesClass != null) {
            if (!"android.os.LocaleList".equals(localesClass)) throw new IllegalArgumentException("Invalid text locales type");
            mLocales = LocaleList.CREATOR.createFromParcel(source);
        }
        mShadowRadius = source.readFloat();
        mShadowDx = source.readFloat();
        mShadowDy = source.readFloat();
        mShadowColor = source.readInt();
        mHasElegantTextHeight = source.readBoolean();
        mElegantTextHeight = source.readBoolean();
        mHasLetterSpacing = source.readBoolean();
        mLetterSpacing = source.readFloat();
        mFontFeatures = source.readString();
        mFontVariation = source.readString();
    }
    public String getFamily() { return mFamily; } public ColorStateList getTextColor() { return mColor; } public ColorStateList getLinkTextColor() { return mLinkColor; } public int getTextSize() { return mSize; } public int getTextStyle() { return mStyle; }
    @Override public void updateDrawState(android.text.TextPaint tp) {
        updateMeasureState(tp);
        if (mColor != null) tp.setColor(mColor.getColorForState(tp.drawableState, 0));
        if (mLinkColor != null) tp.linkColor = mLinkColor.getColorForState(tp.drawableState, 0);
        if (mShadowColor != 0) tp.setShadowLayer(mShadowRadius, mShadowDx, mShadowDy, mShadowColor);
    }
    @Override public void updateMeasureState(android.text.TextPaint tp) {
        if (mFamily != null || mStyle != 0) {
            Typeface t = tp.getTypeface(); int st = (t == null ? 0 : t.getStyle()) | mStyle;
            t = mFamily != null ? Typeface.create(mFamily, st) : Typeface.create(t, st);
            if (mFontWeight >= 0) t = Typeface.create(t, Math.min(1000, mFontWeight), (st & Typeface.ITALIC) != 0);
            tp.setTypeface(t);
        }
        if (mSize > 0) tp.setTextSize(mSize);
        if (mLocales != null) tp.setTextLocales(mLocales);
        if (mHasElegantTextHeight) tp.setElegantTextHeight(mElegantTextHeight);
        if (mHasLetterSpacing) tp.setLetterSpacing(mLetterSpacing);
        if (mFontFeatures != null) tp.setFontFeatureSettings(mFontFeatures);
        if (mFontVariation != null) tp.setFontVariationSettings(mFontVariation);
    }
    public int getSpanTypeId() { return getSpanTypeIdInternal(); }
    public int getSpanTypeIdInternal() { return TextUtils.TEXT_APPEARANCE_SPAN; }
    public void writeToParcel(Parcel dest, int flags) { writeToParcelInternal(dest, flags); }
    public void writeToParcelInternal(Parcel dest, int flags) {
        dest.writeString(mFamily);
        dest.writeInt(mStyle);
        dest.writeInt(mSize);
        dest.writeInt(mColor != null ? 1 : 0);
        if (mColor != null) mColor.writeToParcel(dest, flags);
        dest.writeInt(mLinkColor != null ? 1 : 0);
        if (mLinkColor != null) mLinkColor.writeToParcel(dest, flags);
        // This implementation never holds a resource-backed Typeface; a foreign PID denotes null.
        dest.writeInt(-1);
        dest.writeInt(-1);
        dest.writeInt(mFontWeight);
        dest.writeString(mLocales == null ? null : "android.os.LocaleList");
        if (mLocales != null) mLocales.writeToParcel(dest, flags);
        dest.writeFloat(mShadowRadius);
        dest.writeFloat(mShadowDx);
        dest.writeFloat(mShadowDy);
        dest.writeInt(mShadowColor);
        dest.writeBoolean(mHasElegantTextHeight);
        dest.writeBoolean(mElegantTextHeight);
        dest.writeBoolean(mHasLetterSpacing);
        dest.writeFloat(mLetterSpacing);
        dest.writeString(mFontFeatures);
        dest.writeString(mFontVariation);
    }
    public int describeContents() { return 0; }
    public java.lang.String getFontFeatureSettings() { return mFontFeatures; }
    public java.lang.String getFontVariationSettings() { return mFontVariation; }
    public float getLetterSpacing() { return mLetterSpacing; }
    public int getShadowColor() { return mShadowColor; }
    public float getShadowDx() { return mShadowDx; }
    public float getShadowDy() { return mShadowDy; }
    public float getShadowRadius() { return mShadowRadius; }
    public int getTextFontWeight() { return mFontWeight; }
    public android.os.LocaleList getTextLocales() { return mLocales; }
    public boolean hasElegantTextHeight() { return mHasElegantTextHeight; }
    public boolean hasLetterSpacing() { return mHasLetterSpacing; }
    public boolean isElegantTextHeight() { return mElegantTextHeight; }
    // ---- generated by tools/compat/fillmembers.py: the platform's members this class does not write (signatures only)
    public android.graphics.Typeface getTypeface() { return null; }
    // ---- end of generated members
}
