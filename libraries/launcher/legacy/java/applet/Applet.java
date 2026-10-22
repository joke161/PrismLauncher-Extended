package java.applet;

import java.awt.Dimension;
import java.awt.Panel;
import java.net.URL;

public class Applet extends Panel {
    private static final long serialVersionUID = 1L;
    private AppletStub stub;

    public void setStub(AppletStub stub) {
        this.stub = stub;
    }

    public boolean isActive() {
        if (stub != null) {
            return stub.isActive();
        }
        return false;
    }

    public URL getDocumentBase() {
        return stub != null ? stub.getDocumentBase() : null;
    }

    public URL getCodeBase() {
        return stub != null ? stub.getCodeBase() : null;
    }

    public String getParameter(String name) {
        return stub != null ? stub.getParameter(name) : null;
    }

    public AppletContext getAppletContext() {
        return stub != null ? stub.getAppletContext() : null;
    }

    public void appletResize(int width, int height) {
        resize(width, height);
    }

    public void resize(int width, int height) {
        super.resize(width, height);
    }

    public void resize(Dimension d) {
        super.resize(d);
    }

    public void init() {}
    public void start() {}
    public void stop() {}
    public void destroy() {}
}

