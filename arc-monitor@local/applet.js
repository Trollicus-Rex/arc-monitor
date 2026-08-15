const Applet = imports.ui.applet;
const PopupMenu = imports.ui.popupMenu;
const St = imports.gi.St;
const Gio = imports.gi.Gio;
const Clutter = imports.gi.Clutter;
const GLib = imports.gi.GLib;
const Notify = imports.gi.Notify;

const ArcInterface = `
<node>
    <interface name="org.freedesktop.ArcCtrl">
        <property name="GpuName" type="s" access="read"/>
        <property name="Architecture" type="s" access="read"/>
        <property name="Temperature" type="d" access="read"/>
        <property name="FanRPM" type="u" access="read"/>
        <property name="VramUsed" type="t" access="read"/>
        <property name="VramTotal" type="t" access="read"/>
        <property name="Engines" type="a(sd)" access="read"/>
        <method name="SetPowerLimit">
            <arg type="d" direction="in"/>
            <arg type="b" direction="out"/>
        </method>
        <method name="SetFanPWM">
            <arg type="d" direction="in"/>
            <arg type="b" direction="out"/>
        </method>
        <method name="SetFanAuto">
            <arg type="b" direction="out"/>
        </method>
    </interface>
</node>`;

const ArcProxy = Gio.DBusProxy.makeProxyWrapper(ArcInterface);

class ProgressBarMenuItem extends PopupMenu.PopupBaseMenuItem {
    constructor(label_text) {
        super({ reactive: false });
        
        this.layout = new St.BoxLayout({ vertical: true, x_expand: true });
        this.label = new St.Label({ text: label_text });
        this.track = new St.BoxLayout({ style: 'background-color: rgba(255,255,255,0.1); border-radius: 4px; height: 8px; margin-top: 4px;' });
        this.bar = new St.Bin({ style: 'background-color: #3498db; border-radius: 4px;' });
        
        this.track.add_actor(this.bar);
        this.layout.add_actor(this.label);
        this.layout.add_actor(this.track);
        this.addActor(this.layout, { expand: true });
    }

    setProgress(percent, text, color = '#3498db') {
        this.label.set_text(text);
        this.bar.style = `background-color: ${color}; border-radius: 4px; width: ${Math.max(1, percent * 200)}px;`;
    }
}

// Note: Switched from TextIconApplet to base Applet
class MyApplet extends Applet.Applet {
    constructor(metadata, orientation, panel_height, instance_id) {
        super(orientation, panel_height, instance_id);
        
        this.metadata = metadata;
        this.appletPath = metadata.path;
        
        Notify.init("ArcMonitor");
        this._notifiedHot = false;
        
        this.set_applet_tooltip("Intel Arc Telemetry");

        // --- CUSTOM TOOLBAR (PANEL) UI ---
        // We use Clutter.BinLayout to overlay children directly on top of each other
        this.actor.set_layout_manager(new Clutter.BinLayout());
        
        // The background container holding the dynamic image
        this.panelBg = new St.Bin({
            style: `
                background-size: contain;
                background-position: center;
                background-repeat: no-repeat;
                min-width: 50px;
                min-height: 24px;
            `
        });
        
        // The temperature text, heavily outlined in black to be readable over Steve
        this.panelLabel = new St.Label({
            text: "--°C",
            style: "color: #ffffff; font-weight: bold; font-size: 1.2em; text-shadow: 1px 1px 3px rgba(0,0,0,1), -1px -1px 3px rgba(0,0,0,1), 0px 0px 5px rgba(0,0,0,1);",
            x_align: Clutter.ActorAlign.CENTER,
            y_align: Clutter.ActorAlign.CENTER
        });
        
        this.panelBg.set_child(this.panelLabel);
        this.actor.add_actor(this.panelBg);

        // --- POPUP MENU UI ---
        this.menuManager = new PopupMenu.PopupMenuManager(this);
        this.menu = new Applet.AppletPopupMenu(this, orientation);
        this.menuManager.addMenu(this.menu);

        this._buildUI();
        this._connectToDBus();
    }

    _buildUI() {
        let headerBox = new St.BoxLayout({ vertical: false, style: 'padding-bottom: 10px;' });
        
        this.steveIcon = new St.Icon({
            gicon: this._getSteveImage('steve-normal.png'),
            icon_size: 80,
            style_class: 'steve-container',
            style: 'margin-right: 12px;'
        });
        
        let infoBox = new St.BoxLayout({ vertical: true, y_align: Clutter.ActorAlign.CENTER });
        this.gpuNameLabel = new St.Label({ text: "Searching for Arc GPU...", style: 'font-weight: bold; font-size: 1.1em;' });
        this.archLabel = new St.Label({ text: "Architecture: --", style: 'color: #aaaaaa; font-size: 0.9em;' });
        this.tempLabel = new St.Label({ text: "Temp: -- °C | Fan: -- RPM", style: 'color: #e74c3c; font-weight: bold;' });
        
        infoBox.add_actor(this.gpuNameLabel);
        infoBox.add_actor(this.archLabel);
        infoBox.add_actor(this.tempLabel);
        
        headerBox.add_actor(this.steveIcon);
        headerBox.add_actor(infoBox);
        
        let headerItem = new PopupMenu.PopupBaseMenuItem({ reactive: false });
        headerItem.addActor(headerBox, { expand: true });
        this.menu.addMenuItem(headerItem);
        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());

        this.vramBar = new ProgressBarMenuItem("VRAM Usage");
        this.menu.addMenuItem(this.vramBar);
        
        this.engineBar = new ProgressBarMenuItem("Render Engine");
        this.menu.addMenuItem(this.engineBar);

        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());

        let fanHeader = new PopupMenu.PopupMenuItem("Manual Fan Speed (%)", { reactive: false });
        this.menu.addMenuItem(fanHeader);
        
        this.fanSlider = new PopupMenu.PopupSliderMenuItem(0.0);
        this.fanSlider.connect('drag-end', (slider) => {
            let pct = slider.value * 100.0;
            if (this.proxy) this.proxy.SetFanPWMSync(pct);
        });
        this.menu.addMenuItem(this.fanSlider);

        let powerHeader = new PopupMenu.PopupMenuItem("Power Limit (Watts)", { reactive: false });
        this.menu.addMenuItem(powerHeader);
        
        this.powerSlider = new PopupMenu.PopupSliderMenuItem(0.5); 
        this.powerSlider.connect('drag-end', (slider) => {
            let watts = 30.0 + (slider.value * 220.0);
            powerHeader.label.set_text(`Power Limit (~${Math.round(watts)}W)`);
            if (this.proxy) this.proxy.SetPowerLimitSync(watts);
        });
        this.menu.addMenuItem(this.powerSlider);

        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());

        this.autoFanBtn = new PopupMenu.PopupMenuItem("Reset Fan to Auto Curve");
        this.autoFanBtn.connect('activate', () => {
            if (this.proxy) {
                this.proxy.SetFanAutoSync();
                this.fanSlider.setValue(0.0);
            }
        });
        this.menu.addMenuItem(this.autoFanBtn);
    }

    _getSteveImage(filename) {
        let file = Gio.File.new_for_path(this.appletPath + "/images/" + filename);
        if (file.query_exists(null)) {
            return new Gio.FileIcon({ file: file });
        }
        return Gio.icon_new_for_string("weather-clear");
    }

    _showHotWarning(temp) {
        try {
            let iconPath = this.appletPath + "/images/steve-hot.png";
            let notification = new Notify.Notification({
                summary: "GPU Overheating!",
                body: `Intel Arc temperature has reached ${temp.toFixed(1)}°C!`,
                icon_name: iconPath
            });
            notification.set_urgency(Notify.Urgency.CRITICAL);
            notification.show();
        } catch (e) {
            global.logError("ArcMonitor: Failed to show notification: " + e);
        }
    }

    _connectToDBus() {
        try {
            this.proxy = new ArcProxy(
                Gio.DBus.system, 
                "org.freedesktop.ArcCtrl", 
                "/org/freedesktop/ArcCtrl/card0"
            );

            this._updateUI();
            this.proxy.connect('g-properties-changed', () => this._updateUI());
        } catch (e) {
            global.logError("ArcMonitor: D-Bus connection failed: " + e);
            this.gpuNameLabel.set_text("Daemon disconnected");
        }
    }

    _updateUI() {
        if (!this.proxy) return;

        try {
            let temp = this.proxy.Temperature;
            let fan = this.proxy.FanRPM;
            let vramUsed = Number(this.proxy.VramUsed) / (1024 * 1024);
            let vramTotal = Number(this.proxy.VramTotal) / (1024 * 1024);
            let vramPct = vramTotal > 0 ? (vramUsed / vramTotal) : 0;
            
            let engines = this.proxy.Engines; 
            let renderUtil = 0.0;
            if (engines && engines.length > 0) {
                for (let i = 0; i < engines.length; i++) {
                    if (engines[i][0].includes("RENDER")) {
                        renderUtil = engines[i][1];
                        break;
                    }
                }
            }

            // --- Theme & Notification Logic ---
            let menuImg = 'steve-normal.png';
            let panelImg = 'steve-normal.png';
            let panelWidth = '50px'; // Normal width

            if (temp > 85.0) {
                menuImg = 'steve-hot.png';
                panelImg = 'steve-hot.png';
                
                if (!this._notifiedHot) {
                    this._showHotWarning(temp);
                    this._notifiedHot = true;
                }
            } else if (vramPct > 0.90) {
                menuImg = 'steve-vram.png';
                panelImg = 'steve-vram.png';
            } else if (fan > 2000 || this.fanSlider.value > 0.7) {
                menuImg = 'steve-fan.png'; // Keep square version in menu
                panelImg = 'steve-fan_long.png'; // Target long version for toolbar
                panelWidth = '90px'; // Expand toolbar width dynamically!
            }
            
            if (temp < 80.0) {
                this._notifiedHot = false;
            }

            // Update Popup Menu Icon
            this.steveIcon.set_gicon(this._getSteveImage(menuImg));

            // Update Panel Background Image & Dynamic Width
            this.panelBg.style = `
                background-image: url('file://${this.appletPath}/images/${panelImg}');
                background-size: contain;
                background-position: center;
                background-repeat: no-repeat;
                min-width: ${panelWidth};
                min-height: 24px;
            `;

            // Update Overlay Text
            this.panelLabel.set_text(`${temp.toFixed(0)}°C`);

            // Update Texts
            this.gpuNameLabel.set_text(this.proxy.GpuName || "Intel Arc GPU");
            this.archLabel.set_text(this.proxy.Architecture || "Unknown Arch");
            this.tempLabel.set_text(`Temp: ${temp.toFixed(1)} °C  |  Fan: ${fan} RPM`);

            let vColor = vramPct > 0.85 ? '#e74c3c' : '#9b59b6';
            this.vramBar.setProgress(vramPct, `VRAM: ${Math.round(vramUsed)} / ${Math.round(vramTotal)} MB`, vColor);
            
            let eColor = renderUtil > 80 ? '#e67e22' : '#2ecc71'; 
            this.engineBar.setProgress(renderUtil / 100.0, `Render 3D: ${renderUtil.toFixed(1)}%`, eColor);

        } catch (e) {
            global.logError("ArcMonitor Error in _updateUI: " + e);
        }
    }

    on_applet_clicked() {
        this.menu.toggle();
    }
}

function main(metadata, orientation, panel_height, instance_id) {
    return new MyApplet(metadata, orientation, panel_height, instance_id);
}