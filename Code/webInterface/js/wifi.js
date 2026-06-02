loadNetworkConfig();
// Refresh just the status block periodically so the user can watch
// the state change after Save+Restart without manually reloading.
setInterval(refreshStatusOnly, 5000);

function loadNetworkConfig() {
    var req = new XMLHttpRequest();
    req.open('POST', '/getwifi/');
    req.send();
    req.onreadystatechange = function() {
        if (this.readyState === 4 && this.status === 200) {
            var json = JSON.parse(req.responseText);
            console.log(json);
            document.getElementById('enableAp').checked        = !!json.enableAp;
            document.getElementById('apSsid').value            = json.apSsid    || '';
            document.getElementById('apBssid').value           = json.apBssid   || '';
            document.getElementById('apPwd').value             = json.apPwd     || '';
            document.getElementById('enableWM').checked        = !!json.enableWM;

            document.getElementById('enableStaticIp4').checked = !!json.enableStaticIp4;
            document.getElementById('ip4Address').value         = json.ip4Address        || '';
            document.getElementById('ip4Gateway').value         = json.ip4Gateway        || '';
            document.getElementById('ip4Subnet').value          = json.ip4Subnet         || '';
            document.getElementById('ip4DnsPrimary').value      = json.ip4DnsPrimary     || '';
            document.getElementById('ip4DnsSecondary').value    = json.ip4DnsSecondary   || '';
            document.getElementById('ip4NTP').value             = json.ip4NTP            || '';

            renderStatus(json.status);
        }
    }
}

function refreshStatusOnly() {
    // Cheaper poll: only re-fetches /getwifi/ to update the status block.
    // The form fields are not touched (would clobber an in-progress edit).
    var req = new XMLHttpRequest();
    req.open('POST', '/getwifi/');
    req.timeout = 4000;
    req.send();
    req.onreadystatechange = function() {
        if (this.readyState === 4 && this.status === 200) {
            try {
                var json = JSON.parse(req.responseText);
                renderStatus(json.status);
            } catch (e) { /* ignore */ }
        }
    };
}

function renderStatus(s) {
    const body = document.getElementById('statusBody');
    if (!body) return;
    if (!s) {
        body.innerHTML = '<div class="row"><span class="label muted">no status</span><span></span></div>';
        return;
    }
    const rows = [];

    // Top banner row: status dot + headline. Uses .status-dot from main.css.
    const stateLabel = s.connected
        ? '<span class="status-dot ok"></span>connected to <strong>' +
          escapeHtml(s.ssid || '?') + '</strong>'
        : '<span class="status-dot bad"></span>not connected (status=' +
          s.wifiStatus + ', mode=' + s.mode + ')';
    rows.push(
      '<div class="row" style="grid-template-columns:1fr">' +
        '<span class="value">' + stateLabel + '</span>' +
      '</div>'
    );

    if (s.connected) {
        rows.push(row('LAN IP',     '<code>' + escapeHtml(s.ip) + '</code>'));
        rows.push(row('Gateway',    '<code>' + escapeHtml(s.gateway) + '</code>'));
        rows.push(row('Subnet',     '<code>' + escapeHtml(s.subnet) + '</code>'));
        rows.push(row('DNS',        '<code>' + escapeHtml(s.dns) + '</code>'));
        rows.push(row('BSSID',      '<code style="font-size:.85em">' + escapeHtml(s.bssid) + '</code>'));
        rows.push(row('RSSI',       s.rssi + ' dBm (ch ' + s.channel + ')'));
        rows.push(row('Device MAC', '<code style="font-size:.85em">' + escapeHtml(s.mac) + '</code>'));
    }
    if (s.apIp) {
        rows.push(row('SoftAP IP',
            '<code>' + escapeHtml(s.apIp) + '</code> · ' +
            s.apClients + ' client' + (s.apClients === 1 ? '' : 's')));
    }
    body.innerHTML = rows.join('');
}

function row(label, val) {
    return '<div class="row">' +
             '<span class="label">' + label + '</span>' +
             '<span class="value">' + val + '</span>' +
           '</div>';
}

function saveNetworkConfig() {
    if (!validatePassword('apPwd')) return;

    var json = {
        enableAp:         document.getElementById('enableAp').checked,
        apSsid:           document.getElementById('apSsid').value,
        apBssid:          document.getElementById('apBssid').value.trim().toUpperCase(),
        apPwd:            document.getElementById('apPwd').value,
        enableWM:         document.getElementById('enableWM').checked,
        enableStaticIp4:  document.getElementById('enableStaticIp4').checked,
        ip4Address:       document.getElementById('ip4Address').value,
        ip4Gateway:       document.getElementById('ip4Gateway').value,
        ip4Subnet:        document.getElementById('ip4Subnet').value,
        ip4DnsPrimary:    document.getElementById('ip4DnsPrimary').value,
        ip4DnsSecondary:  document.getElementById('ip4DnsSecondary').value,
        ip4NTP:           document.getElementById('ip4NTP').value
    };

    var req = new XMLHttpRequest();
    req.open('POST', '/setwifi/');
    req.timeout = 6000;
    req.onreadystatechange = function() {
        if (this.readyState !== 4) return;
        document.getElementById('apPwd').value = '<enter password>';
        if (this.status === 200) {
            // Config saved — WiFi changes require a restart to take
            // effect (the firmware only attempts to associate at boot).
            // Offer it now rather than relying on the user to find the
            // hidden /restart/ menu item.
            showRestartPrompt(json.apSsid);
        } else {
            buttonConfirm(
                document.getElementById('save'),
                'save failed (HTTP ' + this.status + ')',
                6
            );
        }
    };
    req.send(JSON.stringify(json));
}

// Modal-style overlay shown after a successful save. Gives the user
// a clear yes/no on whether to restart, and tells them what to expect
// (because the spa's softAP disappears mid-restart, then the device
// either lands on the home WiFi or comes back to AP-mode if it failed).
function showRestartPrompt(targetSsid) {
    // Build the overlay lazily — keeps the page DOM clean until needed.
    let wrap = document.getElementById('restartPromptWrap');
    if (!wrap) {
        wrap = document.createElement('div');
        wrap.id = 'restartPromptWrap';
        wrap.style.cssText =
            'position:fixed;inset:0;background:rgba(0,0,0,.55);' +
            'display:flex;align-items:center;justify-content:center;' +
            'z-index:9999;padding:1rem;';
        wrap.innerHTML =
            '<div style="background:#fff;color:#0f1d2c;max-width:26rem;width:100%;' +
              'border-radius:12px;padding:1.5rem;box-shadow:0 20px 50px -20px rgba(0,0,0,.4)">' +
              '<h2 style="margin:0 0 .5rem;font-size:1.15rem">Configuration saved</h2>' +
              '<p id="restartMsg" style="margin:0 0 1rem;color:#4a5d72;font-size:.95rem;line-height:1.4"></p>' +
              '<div style="display:flex;gap:.5rem;justify-content:flex-end">' +
                '<button id="restartLater" type="button" ' +
                  'style="padding:.55rem 1rem;border:1px solid #dde6ee;background:#fff;' +
                  'color:#4a5d72;border-radius:8px;cursor:pointer">Restart later</button>' +
                '<button id="restartNow" type="button" ' +
                  'style="padding:.55rem 1rem;border:none;background:#0f4677;color:#fff;' +
                  'border-radius:8px;cursor:pointer;font-weight:600">Restart now</button>' +
              '</div>' +
              '<div id="restartStatus" style="margin-top:1rem;font-size:.85rem;color:#0f4677;' +
                'display:none"></div>' +
            '</div>';
        document.body.appendChild(wrap);
        document.getElementById('restartLater').onclick = function() {
            wrap.remove();
        };
        document.getElementById('restartNow').onclick = function() {
            doRestart(targetSsid);
        };
    }
    const msg = targetSsid
        ? 'WiFi changes only take effect after a restart. Click below to restart now and connect to <strong>' + escapeHtml(targetSsid) + '</strong>.'
        : 'Click below to restart the device so your new settings take effect.';
    document.getElementById('restartMsg').innerHTML = msg +
        '<br><br><span style="color:#93a5b8;font-size:.85em">If the device successfully joins your home WiFi, its AP will disappear and it will be reachable on your LAN. If it can\'t connect, the AP will come back.</span>';
}

function doRestart(targetSsid) {
    const btnNow    = document.getElementById('restartNow');
    const btnLater  = document.getElementById('restartLater');
    const status    = document.getElementById('restartStatus');
    btnNow.disabled = true;
    btnLater.disabled = true;
    btnNow.textContent = 'Restarting…';
    status.style.display = '';
    status.innerHTML = 'Sending restart command. The connection will drop in a moment.';

    const req = new XMLHttpRequest();
    req.open('POST', '/restart/');
    req.timeout = 3000;   // restart is near-instant; if it doesn't ack, that's because it already rebooted
    req.onreadystatechange = function() {
        if (this.readyState !== 4) return;
        status.innerHTML =
            'Restart sent. Device is rebooting…<br><br>' +
            '<strong>If it joins ' + escapeHtml(targetSsid || 'your WiFi') + ':</strong> the AP will disappear in 10-20 s and the device will be reachable on your LAN.<br>' +
            '<strong>If it fails:</strong> the <code>Lay-Z-Spa-XXX</code> AP will reappear within 30 s. Rejoin it and check this Connection Status page.';
    };
    req.ontimeout = function() {
        // Timeout is expected — the device may already be rebooting before
        // it can ack the POST. Treat it as success.
        status.innerHTML =
            'Restart command sent (no ack — device is rebooting).<br>' +
            'Rejoin <strong>' + escapeHtml(targetSsid || 'your WiFi') + '</strong> or the device\'s AP in ~20 s.';
    };
    req.send();
}

// ────────────────────────────────────────────────────────────────────
// WiFi scan — populate a clickable list of nearby APs. Each row, when
// clicked, fills the SSID + BSSID fields in the form below. RSSI is
// shown as a bar indicator (▮▮▮▯ etc.) so the strongest AP is visually
// obvious. Duplicate SSIDs (mesh case) stay as separate rows — that's
// exactly what we want the user to see.

function rssiBars(dbm) {
    // RSSI is negative; closer to 0 = stronger. Typical scale:
    //   ≥ -50 → 4 bars, ≥ -60 → 3, ≥ -70 → 2, ≥ -80 → 1, below → 0.
    let bars = 0;
    if (dbm >= -50) bars = 4;
    else if (dbm >= -60) bars = 3;
    else if (dbm >= -70) bars = 2;
    else if (dbm >= -80) bars = 1;
    return '▮'.repeat(bars) + '▯'.repeat(4 - bars);
}

function escapeHtml(s) {
    // SSIDs can legitimately contain characters that break naive innerHTML.
    return String(s).replace(/[&<>"']/g, function(c) {
        return ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'})[c];
    });
}

function scanNetworks() {
    const btn    = document.getElementById('scanBtn');
    const status = document.getElementById('scanStatus');
    const table  = document.getElementById('scanTable');
    const tbody  = document.getElementById('scanResults');

    btn.disabled = true;
    btn.textContent = 'Scanning…';
    status.textContent = 'this takes ~2 seconds';
    tbody.innerHTML = '';

    const req = new XMLHttpRequest();
    req.open('GET', '/scanwifi/');
    req.timeout = 15000;   // scan ~2-4s on ESP8266 + JSON ser + net round-trip; generous on weak adapters
    req.onreadystatechange = function() {
        if (this.readyState !== 4) return;
        btn.disabled = false;
        btn.textContent = 'Scan';
        if (this.status !== 200) {
            status.textContent = 'scan failed (HTTP ' + this.status + ')';
            return;
        }
        let json;
        try { json = JSON.parse(req.responseText); }
        catch (e) { status.textContent = 'bad response'; return; }
        const nets = (json && json.networks) || [];
        if (nets.length === 0) {
            status.textContent = 'no networks found';
            table.style.display = 'none';
            return;
        }
        status.textContent = nets.length + ' network' + (nets.length === 1 ? '' : 's') +
                             ' — click a row to use it';
        nets.forEach(function(n) {
            const tr = document.createElement('tr');
            tr.style.cursor = 'pointer';
            tr.innerHTML =
                '<td>' + escapeHtml(n.ssid || '(hidden)') + '</td>' +
                '<td style="font-family:monospace;font-size:.85em">' + escapeHtml(n.bssid) + '</td>' +
                '<td style="text-align:right;font-family:monospace">' +
                  rssiBars(n.rssi) + ' <span style="color:#888">' + n.rssi + '</span></td>' +
                '<td>' + (n.secure ? '🔒' : '—') + '</td>';
            tr.onclick = function() {
                // Set apSsid first so any change-listeners pick up the SSID
                // before the BSSID makes it look pinned.
                document.getElementById('apSsid').value  = n.ssid || '';
                document.getElementById('apBssid').value = n.bssid;
                document.getElementById('enableAp').checked = true;
                // Visual feedback — highlight the picked row.
                Array.from(tbody.children).forEach(function(r) { r.style.background = ''; });
                tr.style.background = 'rgba(34,211,238,.18)';
            };
            tbody.appendChild(tr);
        });
        table.style.display = '';
    };
    req.ontimeout = function() {
        btn.disabled = false;
        btn.textContent = 'Scan';
        status.textContent = 'scan timed out';
    };
    req.send();
}
