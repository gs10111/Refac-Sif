from flask import Flask, render_template, request, redirect

from app_state import AppState


def create_app(state: AppState) -> Flask:
    app = Flask(__name__)

    @app.route('/')
    def index():
        with state.lock:
            config = state.config
            connections = list(state.connections)
        return render_template('index.html', config=config, connections=connections)

    @app.route('/config', methods=['POST'])
    def update_config():
        try:
            sleep_min    = int(request.form['sleep_min'])
            idle_min     = int(request.form['idle_min'])
            max_acq      = int(request.form['max_acq'])
            cooldown_sec = int(request.form['cooldown_sec'])
        except (KeyError, ValueError):
            return 'Valores invalidos: todos os campos devem ser inteiros.', 400

        if any(v <= 0 for v in [sleep_min, idle_min, max_acq, cooldown_sec]):
            return 'Valores invalidos: todos os campos devem ser maiores que zero.', 400

        with state.lock:
            state.config.sleep_min    = sleep_min
            state.config.idle_min     = idle_min
            state.config.max_acq      = max_acq
            state.config.cooldown_sec = cooldown_sec

        return redirect('/')

    return app


def web_server_main(state: AppState, port: int = 8080) -> None:
    app = create_app(state)
    app.run(host='0.0.0.0', port=port, use_reloader=False)
