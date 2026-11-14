package com.mhgu.relink;

import android.content.ContentResolver;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.net.Uri;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.Arrays;

/** Bounded Android image decode into immutable, template-shared icon content. */
final class TouchIconImport {
    private static final int MAX_INPUT = 16 * 1024 * 1024;
    private static final int ICON_SIZE = 128;

    static String importImage(ContentResolver resolver, Uri uri, File root) throws Exception {
        byte[] source;
        try (InputStream in = resolver.openInputStream(uri);
             ByteArrayOutputStream bytes = new ByteArrayOutputStream()) {
            if (in == null) throw new IOException("image could not be opened");
            byte[] buffer = new byte[8192];
            int count;
            while ((count = in.read(buffer)) != -1) {
                if (bytes.size() > MAX_INPUT - count) throw new IOException("image exceeds 16 MB");
                bytes.write(buffer, 0, count);
            }
            source = bytes.toByteArray();
        }

        BitmapFactory.Options options = new BitmapFactory.Options();
        options.inJustDecodeBounds = true;
        BitmapFactory.decodeByteArray(source, 0, source.length, options);
        int width = options.outWidth, height = options.outHeight;
        if (width <= 0 || height <= 0 || width > 16384 || height > 16384
                || (long) width * height > 64L * 1024 * 1024)
            throw new IOException("unsupported image dimensions");
        options.inJustDecodeBounds = false;
        options.inScaled = false;
        options.inPreferredConfig = Bitmap.Config.ARGB_8888;
        options.inSampleSize = 1;
        while (Math.max(width, height) / options.inSampleSize > ICON_SIZE * 2)
            options.inSampleSize *= 2;
        Bitmap decoded = BitmapFactory.decodeByteArray(source, 0, source.length, options);
        if (decoded == null) throw new IOException("unsupported image");
        Bitmap icon = null;
        byte[] encoded;
        try {
            if (decoded.getWidth() > 512 || decoded.getHeight() > 512)
                throw new IOException("image decoder exceeded its size limit");
            float scale = Math.min(1f, (float) ICON_SIZE / Math.max(decoded.getWidth(), decoded.getHeight()));
            width = Math.max(1, Math.round(decoded.getWidth() * scale));
            height = Math.max(1, Math.round(decoded.getHeight() * scale));
            icon = Bitmap.createScaledBitmap(decoded, width, height, true);
            int[] pixels = new int[width * height];
            icon.getPixels(pixels, 0, width, 0, 0, width, height);
            encoded = new byte[8 + pixels.length * 4];
            encoded[0] = 'T'; encoded[1] = 'I'; encoded[2] = 'C'; encoded[3] = 'O';
            encoded[4] = (byte) width; encoded[5] = (byte) (width >> 8);
            encoded[6] = (byte) height; encoded[7] = (byte) (height >> 8);
            for (int i = 0, at = 8; i < pixels.length; ++i) {
                int pixel = pixels[i];
                encoded[at++] = (byte) (pixel >> 16);
                encoded[at++] = (byte) (pixel >> 8);
                encoded[at++] = (byte) pixel;
                encoded[at++] = (byte) (pixel >>> 24);
            }
        } finally {
            if (icon != null && icon != decoded) icon.recycle();
            decoded.recycle();
        }
        return store(root, encoded);
    }

    private static String store(File root, byte[] encoded) throws Exception {
        byte[] digest = MessageDigest.getInstance("SHA-256").digest(encoded);
        StringBuilder text = new StringBuilder(64);
        final char[] hex = "0123456789abcdef".toCharArray();
        for (byte value : digest) text.append(hex[(value & 255) >> 4]).append(hex[value & 15]);
        String hash = text.toString();
        File directory = new File(root, "touch_icons");
        if (!directory.isDirectory() && !directory.mkdirs()) throw new IOException("icon folder unavailable");
        File target = new File(directory, hash + ".rgba");
        if (target.exists()) {
            if (target.length() != encoded.length || !Arrays.equals(Files.readAllBytes(target.toPath()), encoded))
                throw new IOException("existing icon content does not match its hash");
            return hash;
        }
        File staged = File.createTempFile("import-", ".part", directory);
        try {
            try (FileOutputStream out = new FileOutputStream(staged)) {
                out.write(encoded);
                out.getFD().sync();
            }
            Files.move(staged.toPath(), target.toPath());
        } finally {
            if (staged.exists()) staged.delete();
        }
        return hash;
    }

    private TouchIconImport() {}
}
