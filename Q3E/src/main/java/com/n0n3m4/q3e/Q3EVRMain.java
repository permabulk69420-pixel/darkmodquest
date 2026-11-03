/*
	This file is part of the VR port of The Dark Mod for Meta Quest.

    It is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License,
    or (at your option) any later version.
 */

package com.n0n3m4.q3e;

import android.content.Intent;
import android.os.Bundle;

/**
 * Starts The Dark Mod as a VR application (OpenXR), for Meta Quest.
 *
 * Declared in the manifest as a launcher activity of its own with the "VR" categories, so that Quest puts
 * the app into immersive mode instead of showing it in a 2D window. Apart from that it behaves exactly
 * like starting The Dark Mod from a launcher shortcut: the game data location and the command line are
 * taken from the settings of the launcher app.
 */
public class Q3EVRMain extends Q3EMain
{
    @Override
    public void onCreate(Bundle savedInstanceState)
    {
        Intent intent = getIntent();
        if(intent != null)
            intent.putExtra("game", Q3EGameConstants.GAME_TDM);

        super.onCreate(savedInstanceState);
    }

    @Override
    protected boolean IsVRMode()
    {
        // makes the engine render through OpenXR instead of into the window
        return true;
    }
}
