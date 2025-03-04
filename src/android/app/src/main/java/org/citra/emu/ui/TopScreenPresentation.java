package org.citra.emu.ui;

import android.app.Presentation;
import android.content.Context;
import android.os.Bundle;
import android.view.Display;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import org.citra.emu.NativeLibrary;

public class TopScreenPresentation extends Presentation implements SurfaceHolder.Callback {
    private SurfaceView mSurfaceView;

    public TopScreenPresentation(Context context, Display display) {
        super(context, display);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // Inflate a layout that contains only the SurfaceView for the top screen.
        setContentView(R.layout.presentation_top_screen);
        mSurfaceView = findViewById(R.id.surface_top_screen);
        mSurfaceView.getHolder().addCallback(this);
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        Surface topSurface = holder.getSurface();
        // Pass the top screen surface to the native library.
        NativeLibrary.SetTopScreenSurface(topSurface);
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        // You can handle changes here if needed.
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        // Handle surface destruction if needed.
    }
}