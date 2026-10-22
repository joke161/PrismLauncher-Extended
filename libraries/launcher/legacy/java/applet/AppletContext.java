package java.applet;

import java.awt.Image;
import java.io.InputStream;
import java.net.URL;
import java.util.Enumeration;
import java.util.Iterator;

public interface AppletContext {
    Applet getApplet(String name);
    Enumeration<Applet> getApplets();
    Image getImage(URL url);
    java.applet.AudioClip getAudioClip(URL url);
    void showDocument(URL url);
    void showDocument(URL url, String target);
    void showStatus(String status);
    void setStream(String key, InputStream stream);
    InputStream getStream(String key);
    Iterator<String> getStreamKeys();
}

