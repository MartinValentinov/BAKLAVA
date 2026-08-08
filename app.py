from flask import Flask, render_template

APP_SETTINGS = {
    "project_title": "BAKLAVA",
    "project_subtitle": "BAlKan Location Analisys of Vessels with AI",

    "logo_path": "img/logo.svg",

    "btn_request_label": "SEND REQUEST",
    "btn_download_label": "DOWNLOAD DATA",

    "link_ais_archive": "https://supm.online/ais/",

    "catalogue_title": "SAR Scene Catalogue",
    "catalogue_hint": "Use SEND REQUEST",
    "catalogue_hint_ready": "Use DOWNLOAD DATA",
    "details_title": "VESSELS DETAILS",

    "msg_request_ok":       "Request was successful",
    "msg_request_failed":   "Request failed. Try again.",
    "msg_request_sending":  "Sending request...",
    "msg_download_ok":      "Data downloaded",
    "msg_download_failed":  "Download failed. Try again.",
    "msg_process_sending":  "Processing...",
    "msg_process_ok":       "Processing was successful",
    "msg_process_failed":   "Processing failed. Try again.",

    "map_default_lat": 43.2,
    "map_default_lon": 30.0,
    "map_default_zoom": 5,

    "api_base_url": "http://localhost:5080",
}

app = Flask(__name__)


@app.route("/")
def home():
    return render_template("index.html", settings=APP_SETTINGS)


if __name__ == "__main__":
    app.run(debug=True, port=5000)
