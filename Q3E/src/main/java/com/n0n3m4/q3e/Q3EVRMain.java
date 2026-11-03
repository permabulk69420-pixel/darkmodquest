/*
	This file is part of the VR port of The Dark Mod for Meta Quest.

    It is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License,
    or (at your option) any later version.
 */

package com.n0n3m4.q3e;

import android.Manifest;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.util.Locale;

/**
 * Starts The Dark Mod as a VR application (OpenXR), for Meta Quest.
 *
 * Declared in the manifest as a launcher activity of its own with the "VR" categories, so that Quest puts
 * the app into immersive mode instead of showing it in a 2D window. Apart from that it behaves exactly
 * like starting The Dark Mod from a launcher shortcut: the game data location and the command line are
 * taken from the settings of the launcher app.
 *
 * Since the launcher screen is skipped, this activity has to do what the launcher's "Start" button does
 * before the game may be started: get access to the game files (Android asks the user for it, and it is
 * lost whenever the app is reinstalled), and make sure the game files are really readable. The engine
 * can't cope with a game folder it can't read, it would just die at startup.
 */
public class Q3EVRMain extends Q3EMain
{
    private static final String TAG = "TDM-VR";
    private static final int REQUEST_STORAGE_ACCESS = 0x7d01;

    // the start of the game is postponed until the user has allowed access to the game files
    private boolean m_waitingForStorage = false;
    // the permission screen was in front of this activity (the first onResume comes before that)
    private boolean m_storageScreenShown = false;
    private AlertDialog m_dialog = null;

    @Override
    public void onCreate(Bundle savedInstanceState)
    {
        Intent intent = getIntent();
        if(intent != null)
            intent.putExtra("game", Q3EGameConstants.GAME_TDM);

        super.onCreate(savedInstanceState);

        // PrepareStart() had to postpone the start: ask for the access now that the activity is set up
        if(m_waitingForStorage)
            RequestStorageAccess();
    }

    @Override
    protected boolean IsVRMode()
    {
        // makes the engine render through OpenXR instead of into the window
        return true;
    }

    @Override
    protected void onPause()
    {
        super.onPause();

        if(m_waitingForStorage)
            m_storageScreenShown = true;
    }

    @Override
    protected void onResume()
    {
        super.onResume();

        if(!m_waitingForStorage || !m_storageScreenShown)
            return;
        m_storageScreenShown = false;

        if(HasStorageAccess())
        {
            Log.i(TAG, "Access to the game files was granted, starting the game");
            m_waitingForStorage = false;
            recreate(); // run the whole start again, now that the files can be reached
        }
        else
        {
            Log.w(TAG, "Access to the game files is still not granted");
            ShowStorageAccessDialog();
        }
    }

    @Override
    protected void onDestroy()
    {
        if(null != m_dialog)
        {
            m_dialog.dismiss();
            m_dialog = null;
        }
        super.onDestroy();
    }

    @Override
    protected boolean PrepareStart()
    {
        if(HasStorageAccess())
        {
            Log.i(TAG, "Access to the game files: granted");
            return true;
        }

        Log.w(TAG, "Access to the game files is not granted (app data was reset or the app was reinstalled?), asking for it");
        m_waitingForStorage = true;
        return false;
    }

    @Override
    protected boolean VerifyGameData()
    {
        final String dir = Q3E.q3ei.GetGameDataDirectoryPath(null);
        final String problem = FindGameDataProblem(dir);
        if(null == problem)
        {
            Log.i(TAG, "Game files are readable: " + dir);
            return true;
        }

        Log.e(TAG, "Can't use the game files in " + dir + ": " + problem);
        gameHelper.FatalError("The Dark Mod game files can't be used.\n\n" + problem + "\n\nGame folder:\n" + dir
                + "\n\nRestart opens the idTech4A++ launcher, where the game folder can be changed.");
        return false;
    }

    /** Looks for something that keeps the game files from being used. Returns null if they can be read. */
    private static String FindGameDataProblem(String path)
    {
        File dir = new File(path);
        if(!dir.isDirectory())
            return "The folder does not exist.";

        File[] files = dir.listFiles();
        if(null == files)
            return "The folder can't be listed. The app is probably not allowed to access all files.";

        // The Dark Mod keeps its data in tdm_*.pk4 archives next to the engine
        File pk4 = null;
        for(File file : files)
        {
            if(file.isFile() && file.getName().toLowerCase(Locale.ROOT).endsWith(".pk4"))
            {
                pk4 = file;
                break;
            }
        }
        if(null == pk4)
            return "The folder contains no game data (no .pk4 files)" + (files.length == 0 ? ", it looks empty. If the files are there, the app is not allowed to access all files." : ".");

        // look inside: being listed does not prove that the contents can be read
        FileInputStream in = null;
        try
        {
            in = new FileInputStream(pk4);
            byte[] head = new byte[4];
            if(in.read(head) != head.length)
                return pk4.getName() + " is empty or damaged.";
        }
        catch(IOException e)
        {
            return "Can't read " + pk4.getName() + " (" + e.getMessage() + ").";
        }
        finally
        {
            Q3EUtils.Close(in);
        }
        return null;
    }

    private boolean HasStorageAccess()
    {
        if(Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) // Android 11: "All files access"
            return Environment.isExternalStorageManager();
        if(Build.VERSION.SDK_INT >= Build.VERSION_CODES.M)
            return checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
        return true;
    }

    /** Same as the launcher's "Start" button does: send the user to the system screen where the access is allowed. */
    private void RequestStorageAccess()
    {
        try
        {
            if(Build.VERSION.SDK_INT >= Build.VERSION_CODES.R)
            {
                try
                {
                    Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
                    intent.setData(Uri.parse("package:" + getPackageName()));
                    startActivityForResult(intent, REQUEST_STORAGE_ACCESS);
                }
                catch(Exception e) // not every system has the page of a single app, then show the list of all apps
                {
                    Log.w(TAG, "No settings page for this app, trying the list of all apps", e);
                    startActivityForResult(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION), REQUEST_STORAGE_ACCESS);
                }
            }
            else if(Build.VERSION.SDK_INT >= Build.VERSION_CODES.M)
            {
                requestPermissions(new String[] { Manifest.permission.WRITE_EXTERNAL_STORAGE }, REQUEST_STORAGE_ACCESS);
            }
        }
        catch(Exception e)
        {
            Log.e(TAG, "Can't open the screen to allow access to the game files", e);
            ShowStorageAccessDialog();
        }
    }

    private void ShowStorageAccessDialog()
    {
        // never in the middle of onCreate/onResume, and never twice
        new Handler(Looper.getMainLooper()).post(new Runnable() {
            @Override
            public void run()
            {
                if(isFinishing() || (null != m_dialog && m_dialog.isShowing()))
                    return;

                AlertDialog.Builder builder = new AlertDialog.Builder(Q3EVRMain.this);
                builder.setTitle("Access to the game files is needed");
                builder.setMessage("The Dark Mod needs permission to read its game files (/sdcard/diii4a/darkmod).\n\n"
                        + "Allow \"All files access\" for this app, then come back.\n\n"
                        + "From a computer this also works:\n"
                        + "adb shell appops set " + getPackageName() + " MANAGE_EXTERNAL_STORAGE allow");
                builder.setCancelable(false);
                builder.setPositiveButton("Allow", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which)
                    {
                        dialog.dismiss();
                        RequestStorageAccess();
                    }
                });
                builder.setNegativeButton("Quit", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which)
                    {
                        dialog.dismiss();
                        finish();
                    }
                });
                m_dialog = builder.create();
                m_dialog.show();
            }
        });
    }
}
