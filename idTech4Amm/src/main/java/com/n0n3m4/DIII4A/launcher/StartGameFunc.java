package com.n0n3m4.DIII4A.launcher;

import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.os.Bundle;

import com.karin.idTech4Amm.R;
import com.karin.idTech4Amm.lib.ContextUtility;
import com.n0n3m4.DIII4A.GameLauncher;
import com.n0n3m4.q3e.Q3ELang;
import com.n0n3m4.q3e.Q3EMain;
import com.n0n3m4.q3e.Q3E;
import com.n0n3m4.q3e.Q3EVRMain;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

public final class StartGameFunc extends GameLauncherFunc
{
    private final int m_code;
    private String m_path;

    public StartGameFunc(GameLauncher gameLauncher, int code)
    {
        super(gameLauncher);
        m_code = code;
    }

    public void Reset()
    {
    }

    public void Start(Bundle data)
    {
        super.Start(data);
        Reset();

        m_path = data.getString("path");

        //k check external storage permission
        int res = ContextUtility.CheckFilePermission(m_gameLauncher, m_code);
        if (res == ContextUtility.CHECK_PERMISSION_RESULT_REJECT)
            Toast_long(Tr(R.string.can_t_s_read_write_external_storage_permission_is_not_granted, Tr(R.string.startgame)));
        if (res != ContextUtility.CHECK_PERMISSION_RESULT_GRANTED)
            return;
        run();
    }

    public void run()
    {
        /*
		String dir=m_path;
		if ((new File(dir+"/base").exists())&&(!new File(dir+"/base/gl2progs").exists()))
		getgl2progs(dir+"/base/");
        */

        // On a VR headset The Dark Mod runs in VR; ask whether to start it flat instead (for comparing / troubleshooting)
        if (Q3E.q3ei.isTDM && ContextUtility.IsVRHeadset(m_gameLauncher))
        {
            new AlertDialog.Builder(m_gameLauncher)
                .setTitle("Start The Dark Mod")
                .setPositiveButton("VR", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which)
                    {
                        dialog.dismiss();
                        StartActivity(true);
                    }
                })
                .setNegativeButton("Flat (2D)", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which)
                    {
                        dialog.dismiss();
                        StartActivity(false);
                    }
                })
                .show();
            return;
        }
        StartActivity(false);
    }

    private void StartActivity(boolean vr)
    {
        Intent intent = new Intent(m_gameLauncher, vr ? Q3EVRMain.class : Q3EMain.class);
        if (vr)
            intent.addCategory("com.oculus.intent.category.VR");
        m_gameLauncher.finish();
        m_gameLauncher.startActivity(intent);
    }

    public void getgl2progs(String destname)
    {
        try
        {
            byte[] buf = new byte[4096];
            ZipInputStream zipinputstream = null;
            InputStream bis;
            ZipEntry zipentry;

            bis = m_gameLauncher.getAssets().open("source/gl2progs.zip");
            zipinputstream = new ZipInputStream(bis);

            (new File(destname)).mkdirs();
            while ((zipentry = zipinputstream.getNextEntry()) != null)
            {
                String tmpname = zipentry.getName();

                String entryName = destname + tmpname;
                entryName = entryName.replace('/', File.separatorChar);
                entryName = entryName.replace('\\', File.separatorChar);

                int n;
                FileOutputStream fileoutputstream;
                File newFile = new File(entryName);
                if (zipentry.isDirectory())
                {
                    if (!newFile.mkdirs())
                    {
                    }
                    continue;
                }
                fileoutputstream = new FileOutputStream(entryName);
                while ((n = zipinputstream.read(buf, 0, 4096)) > -1)
                {
                    fileoutputstream.write(buf, 0, n);
                }
                fileoutputstream.close();
                zipinputstream.closeEntry();
            }

            zipinputstream.close();
        }
        catch (Exception e)
        {
            e.printStackTrace();
        }
    }
}
