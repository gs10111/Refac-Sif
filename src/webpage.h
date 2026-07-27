#include <Arduino.h>
const char updatePage[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="pt-BR"><head><meta charset="utf-8"><title>OTA Update</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
:root {
  font-family: "Inter", system-ui, -apple-system, sans-serif;
  color: #101828;
  background: #f4f5fb;
}
body {
  margin: 0;
  min-height: 100vh;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: 1rem;
}
.card {
  width: min(460px, 100%);
  background: #ffffff;
  border-radius: 16px;
  box-shadow: 0 20px 45px rgba(15, 23, 42, 0.15);
  padding: 2rem;
  text-align: center;
}
h2 {
  margin-bottom: 0.25rem;
  font-size: 1.9rem;
}
p {
  margin-top: 0;
  color: #475467;
  font-size: 0.95rem;
}
input[type="file"] {
  width: 100%;
  padding: 0.75rem;
  border-radius: 10px;
  border: 1px dashed #cbd5f5;
  background: #f8fafc;
  cursor: pointer;
  margin-bottom: 1.25rem;
}
button {
  width: 100%;
  border: none;
  border-radius: 12px;
  padding: 0.85rem;
  font-size: 1rem;
  font-weight: 600;
  background: linear-gradient(135deg, #2563eb, #7c3aed);
  color: #fff;
  cursor: pointer;
  transition: transform 0.2s ease, box-shadow 0.2s ease;
}
button:hover {
  transform: translateY(-2px);
  box-shadow: 0 10px 25px rgba(37, 99, 235, 0.35);
}
</style>
</head><body>
<button id="reset-btn" style="position:fixed;top:18px;left:50%;transform:translateX(-50%);z-index:1000;width:auto;min-width:120px;padding:0.7rem 1.5rem;background:linear-gradient(135deg,#ef4444,#f59e42);font-size:1rem;font-weight:600;border:none;border-radius:12px;box-shadow:0 4px 16px rgba(239,68,68,0.15);color:#fff;cursor:pointer;">🔄 Reiniciar ESP</button>
<div class="card">
  <h2>Atualização OTA</h2>
  <p>Envie o binário compilado para atualizar o firmware do Sensor de Força.</p>
  <form id="ota-form" enctype="multipart/form-data">
    <input type="file" name="firmware" id="file-input" accept=".bin" required>
    <div id="drop-area" style="margin-top:1rem; margin-bottom:2.5rem; border:2px dashed #2563eb; border-radius:12px; padding:1.5rem; background:#f8fafc; color:#2563eb; font-size:1.1rem; cursor:pointer;">
      Arraste o arquivo aqui para enviar
    </div>
    <button type="submit">Enviar firmware</button>
  </form>
  <div class="progress"><span id="progress-bar"></span></div>
  <div id="status">Aguardando envio…</div>
</div>
<script>
document.getElementById("reset-btn").onclick = function() {
  fetch("/restartESP", { method: "POST" })
    .then(() => {
      alert("Reiniciando o ESP...");
    })
    .catch(() => {
      alert("Falha ao enviar comando de reinício.");
    });
};
const form = document.getElementById("ota-form");
const bar = document.getElementById("progress-bar");
const status = document.getElementById("status");
const fileInput = document.getElementById("file-input");
const dropArea = document.getElementById("drop-area");

dropArea.addEventListener("dragover", (e) => {
  e.preventDefault();
  dropArea.style.background = "#e0e7ef";
});
dropArea.addEventListener("dragleave", () => {
  dropArea.style.background = "#f8fafc";
});
dropArea.addEventListener("drop", (e) => {
  e.preventDefault();
  dropArea.style.background = "#f8fafc";
  const files = e.dataTransfer.files;
  if (files.length > 0) {
    fileInput.files = files;
  }
});

form.addEventListener("submit", (e) => {
  e.preventDefault();
  const file = fileInput.files[0];
  if (!file) return;
  const xhr = new XMLHttpRequest();
  xhr.open("POST", "/update");
  xhr.upload.onprogress = (event) => {
    if (event.lengthComputable) {
      const pct = Math.round((event.loaded / event.total) * 100);
      bar.style.width = pct + "%";
      status.textContent = `Enviando… ${pct}%`;
    }
  };
  xhr.onload = () => {
    status.textContent = xhr.status === 200 ? "Upload concluído. Reiniciando…" : "Erro no upload.";
  };
  xhr.onerror = () => status.textContent = "Falha no envio.";
  xhr.send(new FormData(form));
});
</script>
</body></html>
)rawliteral";