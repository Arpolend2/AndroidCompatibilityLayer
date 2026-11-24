package android.content;

import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;
import android.text.TextUtils;
import java.util.ArrayList;
import java.util.Map;
import java.util.TreeMap;

/**
 * One operation of a batch (ContentResolver.applyBatch), as Android's: an insert, an update, a delete, an assertion or a
 * call, built by its Builder, with values, selection arguments and extras that may refer back to the results of the
 * operations before it. It was signatures alone, every Builder null: Viber (packages 575 to 645) ended when the user
 * tapped the button of its welcome screen, building ContentProviderOperation.newDelete(...).build() as it reset its data.
 */
public class ContentProviderOperation implements Parcelable {
    public static final int TYPE_INSERT = 1;
    public static final int TYPE_UPDATE = 2;
    public static final int TYPE_DELETE = 3;
    public static final int TYPE_ASSERT = 4;
    public static final int TYPE_CALL = 5;

    private final int mType;
    private final Uri mUri;
    private final String mMethod, mArg;
    private final Map<String, Object> mValues, mExtras;   /* a value may be a BackReference */
    private final String mSelection;
    private final TreeMap<Integer, Object> mSelectionArgs;
    private final Integer mExpectedCount;
    private final boolean mYieldAllowed, mExceptionAllowed;

    private ContentProviderOperation(Builder b) {
        mType = b.mType;
        mUri = b.mUri;
        mMethod = b.mMethod;
        mArg = b.mArg;
        mValues = b.mValues;
        mExtras = b.mExtras;
        mSelection = b.mSelection;
        mSelectionArgs = b.mSelectionArgs;
        mExpectedCount = b.mExpectedCount;
        mYieldAllowed = b.mYieldAllowed;
        mExceptionAllowed = b.mExceptionAllowed;
    }
    /** The same operation on another URI (Android's, for a provider that passes a batch on). */
    public ContentProviderOperation(ContentProviderOperation o, Uri u) {
        mType = o.mType;
        mUri = u;
        mMethod = o.mMethod;
        mArg = o.mArg;
        mValues = o.mValues;
        mExtras = o.mExtras;
        mSelection = o.mSelection;
        mSelectionArgs = o.mSelectionArgs;
        mExpectedCount = o.mExpectedCount;
        mYieldAllowed = o.mYieldAllowed;
        mExceptionAllowed = o.mExceptionAllowed;
    }
    /** Made for Parcel's sake alone; every field empty. */
    public ContentProviderOperation() { this(new Builder(TYPE_DELETE, null)); }

    public static Builder newInsert(Uri u) { return new Builder(TYPE_INSERT, u); }
    public static Builder newUpdate(Uri u) { return new Builder(TYPE_UPDATE, u); }
    public static Builder newDelete(Uri u) { return new Builder(TYPE_DELETE, u); }
    public static Builder newAssertQuery(Uri u) { return new Builder(TYPE_ASSERT, u); }
    public static Builder newCall(Uri u, String method, String arg) { Builder b = new Builder(TYPE_CALL, u); b.mMethod = method; b.mArg = arg; return b; }

    public Uri getUri() { return mUri; }
    public int getType() { return mType; }
    public boolean isYieldAllowed() { return mYieldAllowed; }
    public boolean isExceptionAllowed() { return mExceptionAllowed; }
    public boolean isInsert() { return mType == TYPE_INSERT; }
    public boolean isDelete() { return mType == TYPE_DELETE; }
    public boolean isUpdate() { return mType == TYPE_UPDATE; }
    public boolean isAssertQuery() { return mType == TYPE_ASSERT; }
    public boolean isCall() { return mType == TYPE_CALL; }
    public boolean isWriteOperation() { return mType == TYPE_DELETE || mType == TYPE_INSERT || mType == TYPE_UPDATE; }
    public boolean isReadOperation() { return mType == TYPE_ASSERT; }

    /** Applies it to the provider, as Android's: an exception becomes the result where the builder allowed it. */
    public ContentProviderResult apply(ContentProvider provider, ContentProviderResult[] backRefs, int numBackRefs) throws OperationApplicationException {
        if (!mExceptionAllowed) return applyInternal(provider, backRefs, numBackRefs);
        try {
            return applyInternal(provider, backRefs, numBackRefs);
        } catch (Exception e) {
            return new ContentProviderResult(e);
        }
    }
    private ContentProviderResult applyInternal(ContentProvider provider, ContentProviderResult[] backRefs, int numBackRefs) throws OperationApplicationException {
        ContentValues values = resolveValueBackReferences(backRefs, numBackRefs);
        String[] args = resolveSelectionArgsBackReferences(backRefs, numBackRefs);
        if (mType == TYPE_INSERT) {
            Uri made = provider.insert(mUri, values);
            if (made == null) throw new OperationApplicationException("Insert into " + mUri + " returned no result");
            return new ContentProviderResult(made);
        }
        if (mType == TYPE_CALL) {
            Bundle r = provider.call(mUri.getAuthority(), mMethod, mArg, resolveExtrasBackReferences(backRefs, numBackRefs));
            return new ContentProviderResult(null, null, r, null);
        }
        int rows;
        if (mType == TYPE_DELETE) rows = provider.delete(mUri, mSelection, args);
        else if (mType == TYPE_UPDATE) rows = provider.update(mUri, values, mSelection, args);
        else if (mType == TYPE_ASSERT) {
            // The rows that match must hold the values expected, each read as a string.
            String[] projection = null;
            if (values != null) projection = values.keySet().toArray(new String[0]);
            Cursor c = provider.query(mUri, projection, mSelection, args, null);
            if (c == null) rows = 0;
            else {
                try {
                    rows = c.getCount();
                    if (projection != null) {
                        while (c.moveToNext()) {
                            for (int i = 0; i < projection.length; i++) {
                                String have = c.getString(i), want = values.getAsString(projection[i]);
                                if (!TextUtils.equals(have, want))
                                    throw new OperationApplicationException("Found value " + have + " when expected " + want + " for column " + projection[i]);
                            }
                        }
                    }
                } finally {
                    c.close();
                }
            }
        } else {
            throw new IllegalStateException("bad type, " + mType);
        }
        if (mExpectedCount != null && mExpectedCount != rows) throw new OperationApplicationException("Expected " + mExpectedCount + " rows but actual " + rows);
        return new ContentProviderResult(rows);
    }

    public ContentValues resolveValueBackReferences(ContentProviderResult[] backRefs, int numBackRefs) {
        if (mValues == null) return null;
        ContentValues v = new ContentValues();
        for (Map.Entry<String, Object> e : mValues.entrySet()) {
            Object o = e.getValue();
            v.putObject(e.getKey(), o instanceof BackReference ? ((BackReference) o).resolve(backRefs, numBackRefs) : o);
        }
        return v;
    }
    public Bundle resolveExtrasBackReferences(ContentProviderResult[] backRefs, int numBackRefs) {
        if (mExtras == null) return null;
        Bundle b = new Bundle();
        for (Map.Entry<String, Object> e : mExtras.entrySet()) {
            Object o = e.getValue();
            putBundleValue(b, e.getKey(),
                o instanceof BackReference ? ((BackReference) o).resolve(backRefs, numBackRefs) : o);
        }
        return b;
    }
    private static void putBundleValue(Bundle bundle, String key, Object value) {
        if (value == null || value instanceof String) bundle.putString(key, (String) value);
        else if (value instanceof Boolean) bundle.putBoolean(key, (Boolean) value);
        else if (value instanceof Byte) bundle.putByte(key, (Byte) value);
        else if (value instanceof Short) bundle.putShort(key, (Short) value);
        else if (value instanceof Integer) bundle.putInt(key, (Integer) value);
        else if (value instanceof Long) bundle.putLong(key, (Long) value);
        else if (value instanceof Float) bundle.putFloat(key, (Float) value);
        else if (value instanceof Double) bundle.putDouble(key, (Double) value);
        else if (value instanceof byte[]) bundle.putByteArray(key, (byte[]) value);
        else if (value instanceof android.os.IBinder) bundle.putBinder(key, (android.os.IBinder) value);
        else if (value instanceof Parcelable) bundle.putParcelable(key, (Parcelable) value);
        else if (value instanceof java.io.Serializable) bundle.putSerializable(key, (java.io.Serializable) value);
        else throw new IllegalArgumentException("Unsupported provider operation extra type");
    }
    public String[] resolveSelectionArgsBackReferences(ContentProviderResult[] backRefs, int numBackRefs) {
        if (mSelectionArgs == null) return null;
        String[] args = new String[mSelectionArgs.isEmpty() ? 0 : mSelectionArgs.lastKey() + 1];
        for (Map.Entry<Integer, Object> e : mSelectionArgs.entrySet()) {
            Object o = e.getValue();
            args[e.getKey()] = String.valueOf(o instanceof BackReference ? ((BackReference) o).resolve(backRefs, numBackRefs) : o);
        }
        return args;
    }

    public static String typeToString(int type) {
        switch (type) {
        case TYPE_INSERT: return "insert";
        case TYPE_UPDATE: return "update";
        case TYPE_DELETE: return "delete";
        case TYPE_ASSERT: return "assert";
        case TYPE_CALL: return "call";
        default: return Integer.toString(type);
        }
    }
    @Override public String toString() {
        StringBuilder s = new StringBuilder("ContentProviderOperation(");
        s.append("type=").append(typeToString(mType)).append(' ');
        if (mUri != null) s.append("uri=").append(mUri).append(' ');
        if (mValues != null) s.append("values=").append(mValues).append(' ');
        if (mSelection != null) s.append("selection=").append(mSelection).append(' ');
        if (mSelectionArgs != null) s.append("selectionArgs=").append(mSelectionArgs).append(' ');
        if (mExpectedCount != null) s.append("expectedCount=").append(mExpectedCount).append(' ');
        if (mYieldAllowed) s.append("yieldAllowed ");
        if (mExceptionAllowed) s.append("exceptionAllowed ");
        s.setLength(s.length() - 1);
        return s.append(')').toString();
    }

    private static final int VALUE_NULL = 0, VALUE_STRING = 1, VALUE_BOOLEAN = 2,
        VALUE_BYTE = 3, VALUE_SHORT = 4, VALUE_INTEGER = 5, VALUE_LONG = 6,
        VALUE_FLOAT = 7, VALUE_DOUBLE = 8, VALUE_BLOB = 9, VALUE_BACK_REFERENCE = 10;

    public ContentProviderOperation(Parcel parcel) {
        this(readBuilder(parcel));
    }

    private static Builder readBuilder(Parcel parcel) {
        int type = parcel.readInt();
        if (type < TYPE_INSERT || type > TYPE_CALL)
            throw new IllegalArgumentException("Invalid provider operation type");
        String uriText = readBoundedString(parcel, 8192);
        if (uriText == null) throw new IllegalArgumentException("Provider operation URI is required");
        Builder builder = new Builder(type, Uri.parse(uriText));
        builder.mMethod = readBoundedString(parcel, 4096);
        builder.mArg = readBoundedString(parcel, 4096);
        builder.mSelection = readBoundedString(parcel, 8192);
        int hasExpectedCount = readBoolean(parcel);
        if (hasExpectedCount != 0) builder.mExpectedCount = parcel.readInt();
        builder.mYieldAllowed = readBoolean(parcel) != 0;
        builder.mExceptionAllowed = readBoolean(parcel) != 0;
        builder.mValues = readOperationMap(parcel);
        builder.mExtras = readOperationMap(parcel);
        int selectionCount = parcel.readInt();
        if (selectionCount < -1 || selectionCount > 128)
            throw new IllegalArgumentException("Provider selection arguments exceed limit");
        if (selectionCount >= 0) {
            builder.mSelectionArgs = new TreeMap<>();
            for (int i = 0; i < selectionCount; i++) {
                int index = parcel.readInt();
                if (index < 0 || index >= 128 || builder.mSelectionArgs.containsKey(index))
                    throw new IllegalArgumentException("Invalid provider selection argument index");
                builder.mSelectionArgs.put(index, readOperationValue(parcel));
            }
        }
        return builder;
    }

    private static void writeOperationMap(Parcel parcel, Map<String, Object> values) {
        if (values == null) { parcel.writeInt(-1); return; }
        if (values.size() > 128) throw new IllegalArgumentException("Provider operation values exceed limit");
        parcel.writeInt(values.size());
        for (Map.Entry<String, Object> entry : values.entrySet()) {
            String key = entry.getKey();
            if (key == null || key.isEmpty() || key.length() > 1024)
                throw new IllegalArgumentException("Invalid provider operation key");
            parcel.writeString(key);
            writeOperationValue(parcel, entry.getValue());
        }
    }

    private static Map<String, Object> readOperationMap(Parcel parcel) {
        int count = parcel.readInt();
        if (count < -1 || count > 128) throw new IllegalArgumentException("Provider operation values exceed limit");
        if (count < 0) return null;
        Map<String, Object> values = new java.util.LinkedHashMap<>();
        for (int i = 0; i < count; i++) {
            String key = parcel.readString();
            if (key == null || key.isEmpty() || key.length() > 1024 || values.containsKey(key))
                throw new IllegalArgumentException("Invalid provider operation key");
            values.put(key, readOperationValue(parcel));
        }
        return values;
    }

    private static void writeOperationValue(Parcel parcel, Object value) {
        if (value == null) parcel.writeInt(VALUE_NULL);
        else if (value instanceof String) {
            parcel.writeInt(VALUE_STRING); parcel.writeString(bounded((String) value, 65536));
        } else if (value instanceof Boolean) { parcel.writeInt(VALUE_BOOLEAN); parcel.writeInt((Boolean) value ? 1 : 0); }
        else if (value instanceof Byte) { parcel.writeInt(VALUE_BYTE); parcel.writeInt((Byte) value); }
        else if (value instanceof Short) { parcel.writeInt(VALUE_SHORT); parcel.writeInt((Short) value); }
        else if (value instanceof Integer) { parcel.writeInt(VALUE_INTEGER); parcel.writeInt((Integer) value); }
        else if (value instanceof Long) { parcel.writeInt(VALUE_LONG); parcel.writeLong((Long) value); }
        else if (value instanceof Float) { parcel.writeInt(VALUE_FLOAT); parcel.writeFloat((Float) value); }
        else if (value instanceof Double) { parcel.writeInt(VALUE_DOUBLE); parcel.writeDouble((Double) value); }
        else if (value instanceof byte[]) {
            byte[] bytes = (byte[]) value;
            if (bytes.length > 262144) throw new IllegalArgumentException("Provider operation blob exceeds limit");
            parcel.writeInt(VALUE_BLOB); parcel.writeByteArray(bytes);
        } else if (value instanceof BackReference) {
            BackReference reference = (BackReference) value;
            parcel.writeInt(VALUE_BACK_REFERENCE);
            parcel.writeInt(reference.mFromIndex);
            parcel.writeString(reference.mFromKey);
        } else throw new IllegalArgumentException("Unsupported provider operation value type");
    }

    private static Object readOperationValue(Parcel parcel) {
        switch (parcel.readInt()) {
            case VALUE_NULL: return null;
            case VALUE_STRING: return readBoundedString(parcel, 65536);
            case VALUE_BOOLEAN: {
                int value = readBoolean(parcel);
                return value != 0;
            }
            case VALUE_BYTE: return (byte) parcel.readInt();
            case VALUE_SHORT: return (short) parcel.readInt();
            case VALUE_INTEGER: return parcel.readInt();
            case VALUE_LONG: return parcel.readLong();
            case VALUE_FLOAT: return parcel.readFloat();
            case VALUE_DOUBLE: return parcel.readDouble();
            case VALUE_BLOB: {
                byte[] value = parcel.createByteArray();
                if (value == null || value.length > 262144)
                    throw new IllegalArgumentException("Invalid provider operation blob");
                return value;
            }
            case VALUE_BACK_REFERENCE: {
                int index = parcel.readInt();
                if (index < 0 || index >= 128) throw new IllegalArgumentException("Invalid provider back reference");
                return new BackReference(index, readBoundedString(parcel, 1024));
            }
            default: throw new IllegalArgumentException("Unsupported provider operation value type");
        }
    }

    private static int readBoolean(Parcel parcel) {
        int value = parcel.readInt();
        if (value != 0 && value != 1) throw new IllegalArgumentException("Invalid provider operation boolean");
        return value;
    }

    private static String bounded(String value, int maxLength) {
        if (value.length() > maxLength) throw new IllegalArgumentException("Provider operation string exceeds limit");
        return value;
    }

    private static String readBoundedString(Parcel parcel, int maxLength) {
        String value = parcel.readString();
        return value == null ? null : bounded(value, maxLength);
    }

    public int describeContents() { return 0; }
    public void writeToParcel(Parcel p, int flags) {
        p.writeInt(mType);
        p.writeString(mUri == null ? null : bounded(mUri.toString(), 8192));
        p.writeString(mMethod == null ? null : bounded(mMethod, 4096));
        p.writeString(mArg == null ? null : bounded(mArg, 4096));
        p.writeString(mSelection == null ? null : bounded(mSelection, 8192));
        p.writeInt(mExpectedCount == null ? 0 : 1);
        if (mExpectedCount != null) p.writeInt(mExpectedCount);
        p.writeInt(mYieldAllowed ? 1 : 0);
        p.writeInt(mExceptionAllowed ? 1 : 0);
        writeOperationMap(p, mValues);
        writeOperationMap(p, mExtras);
        if (mSelectionArgs == null) p.writeInt(-1);
        else {
            if (mSelectionArgs.size() > 128) throw new IllegalArgumentException("Provider selection arguments exceed limit");
            p.writeInt(mSelectionArgs.size());
            for (Map.Entry<Integer, Object> entry : mSelectionArgs.entrySet()) {
                if (entry.getKey() < 0 || entry.getKey() >= 128)
                    throw new IllegalArgumentException("Invalid provider selection argument index");
                p.writeInt(entry.getKey());
                writeOperationValue(p, entry.getValue());
            }
        }
    }
    public static final Parcelable.Creator<ContentProviderOperation> CREATOR = new Parcelable.Creator<ContentProviderOperation>() {
        public ContentProviderOperation createFromParcel(Parcel p) { return new ContentProviderOperation(p); }
        public ContentProviderOperation[] newArray(int n) { return new ContentProviderOperation[n]; }
    };

    /** A value taken from an earlier operation's result: its extras' key, else its URI's id, else its count. */
    public static class BackReference implements Parcelable {
        private final int mFromIndex;
        private final String mFromKey;
        private BackReference(int fromIndex, String fromKey) { mFromIndex = fromIndex; mFromKey = fromKey; }
        public BackReference(Parcel p) { BackReference b = (BackReference) p.readValue(null); mFromIndex = b == null ? 0 : b.mFromIndex; mFromKey = b == null ? null : b.mFromKey; }
        BackReference() { this(0, null); }
        public Object resolve(ContentProviderResult[] backRefs, int numBackRefs) {
            if (mFromIndex >= numBackRefs) throw new ArrayIndexOutOfBoundsException("asked for back ref " + mFromIndex + " but there are only " + numBackRefs + " back refs");
            ContentProviderResult r = backRefs[mFromIndex];
            if (r.extras != null) return r.extras.get(mFromKey);
            if (r.uri != null) return ContentUris.parseId(r.uri);
            return r.count == null ? null : (Object) Long.valueOf(r.count);
        }
        @Override public String toString() { return "{fromIndex=" + mFromIndex + ", fromKey=" + mFromKey + "}"; }
        public int describeContents() { return 0; }
        public void writeToParcel(Parcel p, int flags) { p.writeValue(this); }
        public static final Parcelable.Creator<BackReference> CREATOR = new Parcelable.Creator<BackReference>() {
            public BackReference createFromParcel(Parcel p) { return (BackReference) p.readValue(null); }
            public BackReference[] newArray(int n) { return new BackReference[n]; }
        };
    }

    /** Android's builder, with its checks of what each kind of operation may have. */
    public static class Builder {
        private final int mType;
        private final Uri mUri;
        private String mMethod, mArg;
        private Map<String, Object> mValues, mExtras;
        private String mSelection;
        private TreeMap<Integer, Object> mSelectionArgs;
        private Integer mExpectedCount;
        private boolean mYieldAllowed, mExceptionAllowed;

        private Builder(int type, Uri uri) { mType = type; mUri = uri; }
        protected Builder() { this(TYPE_DELETE, null); }

        public ContentProviderOperation build() {
            if (mType == TYPE_UPDATE && (mValues == null || mValues.isEmpty())) throw new IllegalArgumentException("Empty values");
            if (mType == TYPE_ASSERT && (mValues == null || mValues.isEmpty()) && mExpectedCount == null) throw new IllegalArgumentException("Empty values");
            return new ContentProviderOperation(this);
        }
        private void valuesAllowed() {
            if (mType != TYPE_INSERT && mType != TYPE_UPDATE && mType != TYPE_ASSERT) throw new IllegalArgumentException("Values not supported for " + typeToString(mType));
            if (mValues == null) mValues = new java.util.LinkedHashMap<>();
        }
        private void selectionAllowed() {
            if (mType != TYPE_UPDATE && mType != TYPE_DELETE && mType != TYPE_ASSERT) throw new IllegalArgumentException("Selection not supported for " + typeToString(mType));
            if (mSelectionArgs == null) mSelectionArgs = new TreeMap<>();
        }
        private void extrasAllowed() {
            if (mType == TYPE_ASSERT) throw new IllegalArgumentException("Extras not supported for " + typeToString(mType));
            if (mExtras == null) mExtras = new java.util.LinkedHashMap<>();
        }
        public Builder withValues(ContentValues values) {
            valuesAllowed();
            for (String k : values.keySet()) mValues.put(k, values.get(k));
            return this;
        }
        public Builder withValue(String key, Object value) {
            valuesAllowed();
            if (!ContentValues.isSupportedValue(value)) throw new IllegalArgumentException("bad value type: " + value.getClass().getName());
            mValues.put(key, value);
            return this;
        }
        public Builder withValueBackReferences(ContentValues backReferences) {
            valuesAllowed();
            for (String k : backReferences.keySet()) mValues.put(k, new BackReference(backReferences.getAsInteger(k), null));
            return this;
        }
        public Builder withValueBackReference(String key, int fromIndex) { valuesAllowed(); mValues.put(key, new BackReference(fromIndex, null)); return this; }
        public Builder withValueBackReference(String key, int fromIndex, String fromKey) { valuesAllowed(); mValues.put(key, new BackReference(fromIndex, fromKey)); return this; }
        public Builder withExtras(Bundle extras) {
            extrasAllowed();
            for (String k : extras.keySet()) mExtras.put(k, extras.get(k));
            return this;
        }
        public Builder withExtra(String key, Object value) { extrasAllowed(); mExtras.put(key, value); return this; }
        public Builder withExtraBackReference(String key, int fromIndex) { extrasAllowed(); mExtras.put(key, new BackReference(fromIndex, null)); return this; }
        public Builder withExtraBackReference(String key, int fromIndex, String fromKey) { extrasAllowed(); mExtras.put(key, new BackReference(fromIndex, fromKey)); return this; }
        public Builder withSelection(String selection, String[] selectionArgs) {
            selectionAllowed();
            mSelection = selection;
            if (selectionArgs == null) mSelectionArgs = null;
            else for (int i = 0; i < selectionArgs.length; i++) mSelectionArgs.put(i, selectionArgs[i]);
            return this;
        }
        public Builder withSelectionBackReference(int index, int fromIndex) { selectionAllowed(); mSelectionArgs.put(index, new BackReference(fromIndex, null)); return this; }
        public Builder withSelectionBackReference(int index, int fromIndex, String fromKey) { selectionAllowed(); mSelectionArgs.put(index, new BackReference(fromIndex, fromKey)); return this; }
        public Builder withExpectedCount(int count) {
            if (mType != TYPE_UPDATE && mType != TYPE_DELETE && mType != TYPE_ASSERT) throw new IllegalArgumentException("Expected count not supported for " + typeToString(mType));
            mExpectedCount = count;
            return this;
        }
        public Builder withYieldAllowed(boolean yieldAllowed) { mYieldAllowed = yieldAllowed; return this; }
        public Builder withExceptionAllowed(boolean exceptionAllowed) { mExceptionAllowed = exceptionAllowed; return this; }
        public Builder withFailureAllowed(boolean failureAllowed) { return withExceptionAllowed(failureAllowed); }
    }
}
