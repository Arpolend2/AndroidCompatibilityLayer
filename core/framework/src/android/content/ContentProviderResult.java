package android.content;

import android.net.Uri;
import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;

/** What one operation of a batch gave, as Android's: an insert's URI, a count of rows, a call's extras, or what it threw. */
public class ContentProviderResult implements Parcelable {
    public final Uri uri;
    public final Integer count;
    public final Bundle extras;
    public final Throwable exception;

    public ContentProviderResult(Uri uri) { this(java.util.Objects.requireNonNull(uri), null, null, null); }
    public ContentProviderResult(int count) { this(null, count, null, null); }
    public ContentProviderResult(Bundle extras) { this(null, null, java.util.Objects.requireNonNull(extras), null); }
    public ContentProviderResult(Throwable exception) { this(null, null, null, exception); }
    public ContentProviderResult(Uri uri, Integer count, Bundle extras, Throwable exception) {
        this.uri = uri;
        this.count = count;
        this.extras = extras;
        this.exception = exception;
    }
    /** Android's, for another user; there is one here. */
    public ContentProviderResult(ContentProviderResult r, int userId) { this(r.uri, r.count, r.extras, r.exception); }
    public ContentProviderResult(Parcel p) { this(read(p), 0); }
    private static ContentProviderResult read(Parcel p) {
        String uriText = p.readString();
        if (uriText != null && uriText.length() > 8192)
            throw new IllegalArgumentException("Invalid provider result URI");
        int hasCount = p.readInt();
        if (hasCount != 0 && hasCount != 1)
            throw new IllegalArgumentException("Invalid provider result count");
        Integer count = hasCount == 0 ? null : p.readInt();
        Bundle extras = p.readBundle(Bundle.class.getClassLoader());
        if (extras != null && extras.size() > 128)
            throw new IllegalArgumentException("Invalid provider result extras");
        String type = p.readString(), message = p.readString();
        if (type != null && (type.isEmpty() || type.length() > 512) ||
                message != null && (type == null || message.length() > 4096)) {
            throw new IllegalArgumentException("Invalid provider result exception");
        }
        Throwable exception = type == null ? null : new RuntimeException("Remote provider exception " + type +
            (message == null ? "" : ": " + message));
        return new ContentProviderResult(uriText == null ? null : Uri.parse(uriText), count, extras, exception);
    }
    public ContentProviderResult() { this(null, null, null, null); }
    @Override public String toString() {
        StringBuilder s = new StringBuilder("ContentProviderResult(");
        if (uri != null) s.append("uri=").append(uri).append(' ');
        if (count != null) s.append("count=").append(count).append(' ');
        if (extras != null) s.append("extras=").append(extras).append(' ');
        if (exception != null) s.append("exception=").append(exception).append(' ');
        s.setLength(s.length() - 1);
        return s.append(')').toString();
    }
    public int describeContents() { return 0; }
    public void writeToParcel(Parcel p, int flags) {
        String value = uri == null ? null : uri.toString();
        if (value != null && value.length() > 8192)
            throw new IllegalArgumentException("Provider result URI exceeds limit");
        if (extras != null && extras.size() > 128)
            throw new IllegalArgumentException("Provider result extras exceed limit");
        String message = exception == null ? null : exception.getMessage();
        if (message != null && message.length() > 4096) message = message.substring(0, 4096);
        p.writeString(value);
        p.writeInt(count == null ? 0 : 1);
        if (count != null) p.writeInt(count);
        p.writeBundle(extras);
        p.writeString(exception == null ? null : exception.getClass().getName());
        p.writeString(message);
    }
    public static final Parcelable.Creator<ContentProviderResult> CREATOR = new Parcelable.Creator<ContentProviderResult>() {
        public ContentProviderResult createFromParcel(Parcel p) { return new ContentProviderResult(p); }
        public ContentProviderResult[] newArray(int n) { return new ContentProviderResult[n]; }
    };
}
