.class public Lmegler/CameraStorageFixture;
.super Landroid/app/Activity;

.method public constructor <init>()V
    .locals 0
    invoke-direct {p0}, Landroid/app/Activity;-><init>()V
    return-void
.end method

.method private static fail()V
    .locals 2
    new-instance v0, Ljava/lang/AssertionError;
    const-string v1, "Camera MediaStore identity regression"
    invoke-direct {v0, v1}, Ljava/lang/AssertionError;-><init>(Ljava/lang/Object;)V
    throw v0
.end method

.method private static same(Landroid/net/Uri;Landroid/net/Uri;)V
    .locals 1
    if-eqz p0, :bad
    invoke-virtual {p0, p1}, Landroid/net/Uri;->equals(Ljava/lang/Object;)Z
    move-result v0
    if-eqz v0, :bad
    return-void
:bad
    invoke-static {}, Lmegler/CameraStorageFixture;->fail()V
    return-void
.end method

.method private static count(Landroid/content/Context;Ljava/lang/String;)I
    .locals 6
    invoke-virtual {p0}, Landroid/content/Context;->getContentResolver()Landroid/content/ContentResolver;
    move-result-object v0
    sget-object v1, Landroid/provider/MediaStore$Images$Media;->EXTERNAL_CONTENT_URI:Landroid/net/Uri;
    const/4 v2, 0x1
    new-array v4, v2, [Ljava/lang/String;
    const/4 v2, 0x0
    aput-object p1, v4, v2
    const-string v3, "_display_name = ?"
    const/4 v5, 0x0
    invoke-virtual/range {v0 .. v5}, Landroid/content/ContentResolver;->query(Landroid/net/Uri;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;)Landroid/database/Cursor;
    move-result-object v0
    if-eqz v0, :bad
    invoke-interface {v0}, Landroid/database/Cursor;->getCount()I
    move-result v1
    invoke-interface {v0}, Landroid/database/Cursor;->close()V
    return v1
:bad
    invoke-static {}, Lmegler/CameraStorageFixture;->fail()V
    const/4 v0, -0x1
    return v0
.end method

.method private static verify(Landroid/content/Context;Landroid/net/Uri;Ljava/lang/String;)V
    .locals 2
    invoke-static {p0, p2}, Lmegler/CameraStorageFixture;->count(Landroid/content/Context;Ljava/lang/String;)I
    move-result v0
    const/4 v1, 0x1
    if-ne v0, v1, :bad
    invoke-virtual {p0}, Landroid/content/Context;->getContentResolver()Landroid/content/ContentResolver;
    move-result-object v0
    const-string v1, "r"
    invoke-virtual {v0, p1, v1}, Landroid/content/ContentResolver;->openFileDescriptor(Landroid/net/Uri;Ljava/lang/String;)Landroid/os/ParcelFileDescriptor;
    move-result-object v0
    if-eqz v0, :bad
    invoke-virtual {v0}, Landroid/os/ParcelFileDescriptor;->close()V
    return-void
:bad
    invoke-static {}, Lmegler/CameraStorageFixture;->fail()V
    return-void
.end method

.method private static copy(Landroid/content/Context;Ljava/io/File;)V
    .locals 6
    invoke-virtual {p1}, Ljava/io/File;->getParentFile()Ljava/io/File;
    move-result-object v5
    invoke-virtual {v5}, Ljava/io/File;->mkdirs()Z
    invoke-virtual {p0}, Landroid/content/Context;->getAssets()Landroid/content/res/AssetManager;
    move-result-object v0
    const-string v1, "fixture.png"
    invoke-virtual {v0, v1}, Landroid/content/res/AssetManager;->open(Ljava/lang/String;)Ljava/io/InputStream;
    move-result-object v0
    new-instance v1, Ljava/io/FileOutputStream;
    invoke-direct {v1, p1}, Ljava/io/FileOutputStream;-><init>(Ljava/io/File;)V
    const/16 v2, 0x400
    new-array v2, v2, [B
:next
    invoke-virtual {v0, v2}, Ljava/io/InputStream;->read([B)I
    move-result v3
    if-lez v3, :done
    const/4 v4, 0x0
    invoke-virtual {v1, v2, v4, v3}, Ljava/io/FileOutputStream;->write([BII)V
    goto :next
:done
    invoke-virtual {v1}, Ljava/io/FileOutputStream;->close()V
    invoke-virtual {v0}, Ljava/io/InputStream;->close()V
    return-void
.end method

.method public onCreate(Landroid/os/Bundle;)V
    .locals 6
    invoke-super {p0, p1}, Landroid/app/Activity;->onCreate(Landroid/os/Bundle;)V
    sget-object v0, Lmegler/MediaProvider;->INSTANCE:Lmegler/MediaProvider;
    const-string v1, "/sdcard/DCIM/OpenCamera/fixture.png"
    invoke-virtual {v0, v1}, Lmegler/MediaProvider;->scanPath(Ljava/lang/String;)Landroid/net/Uri;
    move-result-object v4
    const-string v1, "/storage/emulated/0/Pictures/DCIM/OpenCamera/fixture.png"
    invoke-virtual {v0, v1}, Lmegler/MediaProvider;->scanPath(Ljava/lang/String;)Landroid/net/Uri;
    move-result-object v5
    invoke-static {v4, v5}, Lmegler/CameraStorageFixture;->same(Landroid/net/Uri;Landroid/net/Uri;)V
    const-string v1, "fixture.png"
    invoke-static {p0, v4, v1}, Lmegler/CameraStorageFixture;->verify(Landroid/content/Context;Landroid/net/Uri;Ljava/lang/String;)V

    new-instance v0, Ljava/io/File;
    const-string v1, "/sdcard/DCIM/OpenCamera/fixture.png"
    invoke-direct {v0, v1}, Ljava/io/File;-><init>(Ljava/lang/String;)V
    invoke-virtual {v0}, Ljava/io/File;->delete()Z
    move-result v2
    if-eqz v2, :bad
    sget-object v0, Lmegler/MediaProvider;->INSTANCE:Lmegler/MediaProvider;
    invoke-virtual {v0, v1}, Lmegler/MediaProvider;->scanPath(Ljava/lang/String;)Landroid/net/Uri;
    move-result-object v2
    if-nez v2, :bad
    const-string v1, "/sdcard/DCIM/OpenCamera"
    invoke-virtual {v0, v1}, Lmegler/MediaProvider;->scanPath(Ljava/lang/String;)Landroid/net/Uri;
    const-string v1, "fixture.png"
    invoke-static {p0, v1}, Lmegler/CameraStorageFixture;->count(Landroid/content/Context;Ljava/lang/String;)I
    move-result v0
    if-nez v0, :bad
    invoke-virtual {p0}, Landroid/app/Activity;->getContentResolver()Landroid/content/ContentResolver;
    move-result-object v0
    const-string v1, "r"
:deleted_start
    invoke-virtual {v0, v4, v1}, Landroid/content/ContentResolver;->openFileDescriptor(Landroid/net/Uri;Ljava/lang/String;)Landroid/os/ParcelFileDescriptor;
:deleted_end
    .catch Ljava/io/FileNotFoundException; { :deleted_start .. :deleted_end } :deleted_ok
    move-result-object v0
    if-eqz v0, :bad
    invoke-virtual {v0}, Landroid/os/ParcelFileDescriptor;->close()V
    goto/16 :bad
:deleted_ok
    move-exception v0
    const-wide/16 v0, 0x44c
    invoke-static {v0, v1}, Ljava/lang/Thread;->sleep(J)V
    new-instance v0, Ljava/io/File;
    const-string v1, "/sdcard/DCIM/OpenCamera/fixture.png"
    invoke-direct {v0, v1}, Ljava/io/File;-><init>(Ljava/lang/String;)V
    invoke-static {p0, v0}, Lmegler/CameraStorageFixture;->copy(Landroid/content/Context;Ljava/io/File;)V
    sget-object v0, Lmegler/MediaProvider;->INSTANCE:Lmegler/MediaProvider;
    invoke-virtual {v0, v1}, Lmegler/MediaProvider;->scanPath(Ljava/lang/String;)Landroid/net/Uri;
    move-result-object v4
    const-string v1, "/storage/emulated/0/Pictures/DCIM/OpenCamera/fixture.png"
    invoke-virtual {v0, v1}, Lmegler/MediaProvider;->scanPath(Ljava/lang/String;)Landroid/net/Uri;
    move-result-object v5
    invoke-static {v4, v5}, Lmegler/CameraStorageFixture;->same(Landroid/net/Uri;Landroid/net/Uri;)V
    const-string v1, "/sdcard/DCIM/OpenCamera"
    invoke-virtual {v0, v1}, Lmegler/MediaProvider;->scanPath(Ljava/lang/String;)Landroid/net/Uri;
    const-string v1, "fixture.png"
    invoke-static {p0, v4, v1}, Lmegler/CameraStorageFixture;->verify(Landroid/content/Context;Landroid/net/Uri;Ljava/lang/String;)V

    const-wide/16 v0, 0x44c
    invoke-static {v0, v1}, Ljava/lang/Thread;->sleep(J)V
    invoke-virtual {p0}, Landroid/app/Activity;->getContentResolver()Landroid/content/ContentResolver;
    move-result-object v0
    sget-object v1, Landroid/provider/MediaStore$Images$Media;->EXTERNAL_CONTENT_URI:Landroid/net/Uri;
    new-instance v2, Landroid/content/ContentValues;
    invoke-direct {v2}, Landroid/content/ContentValues;-><init>()V
    const-string v3, "_display_name"
    const-string v4, "managed.png"
    invoke-virtual {v2, v3, v4}, Landroid/content/ContentValues;->put(Ljava/lang/String;Ljava/lang/String;)V
    const-string v3, "mime_type"
    const-string v4, "image/png"
    invoke-virtual {v2, v3, v4}, Landroid/content/ContentValues;->put(Ljava/lang/String;Ljava/lang/String;)V
    const-string v3, "relative_path"
    const-string v4, "DCIM/OpenCamera"
    invoke-virtual {v2, v3, v4}, Landroid/content/ContentValues;->put(Ljava/lang/String;Ljava/lang/String;)V
    invoke-virtual {v0, v1, v2}, Landroid/content/ContentResolver;->insert(Landroid/net/Uri;Landroid/content/ContentValues;)Landroid/net/Uri;
    move-result-object v4
    new-instance v5, Ljava/io/File;
    const-string v3, "/sdcard/DCIM/OpenCamera/managed.png"
    invoke-direct {v5, v3}, Ljava/io/File;-><init>(Ljava/lang/String;)V
    invoke-static {p0, v5}, Lmegler/CameraStorageFixture;->copy(Landroid/content/Context;Ljava/io/File;)V
    sget-object v0, Lmegler/MediaProvider;->INSTANCE:Lmegler/MediaProvider;
    invoke-virtual {v0, v3}, Lmegler/MediaProvider;->scanPath(Ljava/lang/String;)Landroid/net/Uri;
    move-result-object v5
    invoke-static {v4, v5}, Lmegler/CameraStorageFixture;->same(Landroid/net/Uri;Landroid/net/Uri;)V
    const-string v1, "/sdcard/DCIM/OpenCamera"
    invoke-virtual {v0, v1}, Lmegler/MediaProvider;->scanPath(Ljava/lang/String;)Landroid/net/Uri;
    const-string v1, "managed.png"
    invoke-static {p0, v4, v1}, Lmegler/CameraStorageFixture;->verify(Landroid/content/Context;Landroid/net/Uri;Ljava/lang/String;)V

    new-instance v0, Ljava/io/File;
    const-string v1, "/sdcard/Pictures/Misc/control.png"
    invoke-direct {v0, v1}, Ljava/io/File;-><init>(Ljava/lang/String;)V
    invoke-static {p0, v0}, Lmegler/CameraStorageFixture;->copy(Landroid/content/Context;Ljava/io/File;)V
    sget-object v0, Lmegler/MediaProvider;->INSTANCE:Lmegler/MediaProvider;
    invoke-virtual {v0, v1}, Lmegler/MediaProvider;->scanPath(Ljava/lang/String;)Landroid/net/Uri;
    move-result-object v4
    const-string v1, "/sdcard/Pictures"
    invoke-virtual {v0, v1}, Lmegler/MediaProvider;->scanPath(Ljava/lang/String;)Landroid/net/Uri;
    const-string v1, "control.png"
    invoke-static {p0, v4, v1}, Lmegler/CameraStorageFixture;->verify(Landroid/content/Context;Landroid/net/Uri;Ljava/lang/String;)V
    const-string v0, "CameraStorageFixture"
    const-string v1, "CAMERA_MEDIASTORE_DEDUP_OK"
    invoke-static {v0, v1}, Landroid/util/Log;->i(Ljava/lang/String;Ljava/lang/String;)I
    invoke-virtual {p0}, Landroid/app/Activity;->finish()V
    return-void
:bad
    invoke-static {}, Lmegler/CameraStorageFixture;->fail()V
    return-void
.end method
