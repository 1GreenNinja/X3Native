// widget.js — polls fleet-status.json every 60s, renders the machine grid.
//
// Lives inside an Element iframe widget. Element passes the homeserver URL
// via the widget config; the widget fetches /fleet-status.json from the
// same origin. CORS is fine because the JSON is served by the same
// Cloudflare Tunnel that fronts Element Web.

(function () {
  'use strict';

  const POLL_MS = 60_000;
  const SOURCE_URL = 'fleet-status.json';   // relative — served from same origin

  // Tiny DOM helpers (no framework — this stays under 2 KB minified).
  function el(tag, cls, txt) {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (txt !== undefined) e.textContent = txt;
    return e;
  }
  function $(id) { return document.getElementById(id); }
  function clear(node) { while (node.firstChild) node.removeChild(node.firstChild); }

  function agoText(iso) {
    if (!iso) return 'never';
    const seconds = Math.max(0, Math.floor((Date.now() - new Date(iso).getTime()) / 1000));
    if (seconds < 60) return `${seconds}s ago`;
    if (seconds < 3600) return `${Math.floor(seconds / 60)}m ago`;
    if (seconds < 86400) return `${Math.floor(seconds / 3600)}h ago`;
    return `${Math.floor(seconds / 86400)}d ago`;
  }

  function render(data) {
    $('error').hidden = true;
    $('hdr-meta').textContent = `· updated ${agoText(data.generated_at)}`;

    // Warnings
    const wContainer = $('warnings');
    clear(wContainer);
    if (data.warnings && data.warnings.length) {
      for (const w of data.warnings) {
        const item = el('div', `warn-item ${w.severity || 'info'}`);
        item.textContent = w.text;
        wContainer.appendChild(item);
      }
      wContainer.hidden = false;
    } else {
      wContainer.hidden = true;
    }

    // Machines
    const mContainer = $('machines');
    clear(mContainer);
    for (const m of (data.machines || [])) {
      const node = el('div', 'machine');
      node.setAttribute('data-name', m.name);

      const row1 = el('div', 'machine-row1');
      row1.appendChild(el('span', `dot ${m.presence || 'unknown'}`));
      row1.appendChild(el('span', 'machine-name', m.display_name || m.name.toUpperCase()));

      const branch = (m.branches && m.branches.length) ? m.branches[0] : null;
      if (branch) {
        const b = el('span', 'machine-branch');
        b.appendChild(document.createTextNode(branch.name + ' @ '));
        b.appendChild(el('span', 'branch-sha', branch.head_sha));
        b.appendChild(document.createTextNode(` · ${agoText(branch.head_committed_at)}`));
        row1.appendChild(b);
      } else {
        const b = el('span', 'machine-branch', '(no recent branch)');
        row1.appendChild(b);
      }
      node.appendChild(row1);

      const row2 = el('div', 'machine-row2');
      const role = el('span', 'role-tag', (m.role || 'worker') + ' · ');
      row2.appendChild(role);
      row2.appendChild(document.createTextNode(`${m.lan_ip} · ${m.notes || ''}`));
      node.appendChild(row2);

      if (branch && branch.behind_main > 0) {
        const bw = el('div', 'behind-warn', `⚠ ${branch.behind_main} commits behind main — rebase needed`);
        node.appendChild(bw);
      }
      mContainer.appendChild(node);
    }

    // Integration queue
    const qSection = $('queue-section');
    const qContainer = $('queue');
    clear(qContainer);
    if (data.integration_queue && data.integration_queue.length) {
      for (const q of data.integration_queue) {
        const item = el('div', 'queue-item');
        item.appendChild(el('span', 'arrow', '▸'));
        item.appendChild(document.createTextNode(`${q.branch} (${q.from_machine.toUpperCase()}) `));
        if (q.status_blurb) {
          item.appendChild(el('span', 'blurb', `— ${q.status_blurb}`));
        }
        qContainer.appendChild(item);
      }
      qSection.hidden = false;
    } else {
      qSection.hidden = true;
    }
  }

  function poll() {
    fetch(SOURCE_URL + '?t=' + Date.now(), { cache: 'no-store' })
      .then(r => {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.json();
      })
      .then(render)
      .catch(err => {
        $('error').hidden = false;
        $('error').lastElementChild.textContent = `(${err.message})`;
      });
  }

  poll();
  setInterval(poll, POLL_MS);
})();
