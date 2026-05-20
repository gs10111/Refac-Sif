from pybaselines import Baseline
import pandas as pd
import matplotlib.pyplot as plt 
import numpy as np
from scipy.signal import detrend
from scipy import integrate
from scipy import signal

PATH_DIR = "C:/Users/romeu/OneDrive/Área de Trabalho/Trabalho/SIF-DI241794-SISTEMA-INERCIAL-COM-HADRWARE-FLEX-VEL/192.168.1.110_20250612_164958.csv"
try:
    df = pd.read_csv(PATH_DIR)
    print("Arquivo CSV carregado com sucesso. Primeiras linhas:")
    print(df.head())
except FileNotFoundError:
    print(f"Erro: Arquivo não encontrado em {PATH_DIR}")
    print("Verifique o caminho do arquivo e tente novamente.")
    exit() 
except Exception as e:
    print(f"Erro ao ler o arquivo CSV: {e}")
    exit()

# Parâmetros de conversão
RANGE_ACC = 16   # RANGE em G
RANGE_GYRO = 2000   # RANGE em dps
CONVERSION_FACTOR_ACC = ((2 * RANGE_ACC) / (2**15)) * 9.81   # Fator de conversão para m/s²
CONVERSION_FACTOR_GYRO = ((2 * RANGE_GYRO) / (2**15))   # Fator de conversão para °/s
DT = 0.02
BELT_SPEED = 6   # m/s
BELT_LENGTH = 1000   # m

# Parâmetros filtro passa-baixa
LOWPASS_CUTOFF_ACC = 5
LOWPASS_CUTOFF_GYRO = 5
FILTER_ORDER_ACC = 4
FILTER_ORDER_GYRO = 4

# X axis base: distância = 1, tempo = 2, amostra = 3,
AXIS_SELECTION = 3

def get_x_base_label(axis):
    if axis == 1:
        return "distância[m]"
    elif axis == 2:
        return "tempo[s]"
    elif axis == 3:
        return "amostra"

x_base_label = get_x_base_label(AXIS_SELECTION)

def filter_signal(data, cutoff, order, dt):
    fs = 1 / dt
    lowpass_filter = signal.butter(order, cutoff, btype='lowpass', fs=fs, output='sos')
    filtered_signal = signal.sosfilt(lowpass_filter, data)
    return filtered_signal

def process_sensor_data(acc_signal, gyro_signal):

    sample = np.arange(len(acc_signal))
    acc_signal_ms2 = np.array(acc_signal, dtype=float) * CONVERSION_FACTOR_ACC
    gyro_signal_dps = np.array(gyro_signal, dtype=float) * CONVERSION_FACTOR_GYRO

    # Filtro passa-baixa
    acc_signal_filtered = filter_signal(acc_signal_ms2, LOWPASS_CUTOFF_ACC, FILTER_ORDER_ACC, DT)
    gyro_signal_filtered = filter_signal(gyro_signal_dps, LOWPASS_CUTOFF_GYRO, FILTER_ORDER_GYRO, DT)

    # Detrend
    acc_signal_detrended = detrend(acc_signal_filtered, type='constant')
    gyro_signal_detrended = detrend(gyro_signal_filtered, type='constant')

    # Calculo de velocidade
    speed = integrate.cumulative_trapezoid(acc_signal_detrended, dx=DT, initial=0)
    baseline_fitter_speed = Baseline(x_data=sample)
    half_window = 25
    speed_drift, _ = baseline_fitter_speed.std_distribution(speed, half_window, smooth_half_window=10)
    speed_corrected = speed - speed_drift

    # Calculo de ângulo
    degree = integrate.cumulative_trapezoid(gyro_signal_detrended, dx=DT, initial=0)
    baseline_fitter_degree = Baseline(x_data=sample)
    degree_drift, _ = baseline_fitter_degree.std_distribution(degree, half_window, smooth_half_window=10)
    degree_corrected = degree - degree_drift

    if x_base_label == "distância[m]":
        sample_x_axis = DT * BELT_SPEED * sample
    elif x_base_label == "tempo[s]":
        sample_x_axis = DT * sample
    else:
        sample_x_axis = sample

    # Monta dataframes
    acc_data_df = pd.DataFrame({x_base_label: sample_x_axis, "m/s²": acc_signal_detrended})
    speed_data_df = pd.DataFrame({x_base_label: sample_x_axis, "m/s": speed_corrected})
    gyro_data_df = pd.DataFrame({x_base_label: sample_x_axis, "°/s": gyro_signal_detrended})
    degree_data_df = pd.DataFrame({x_base_label: sample_x_axis, "°": degree_corrected})

    return acc_data_df, speed_data_df, gyro_data_df, degree_data_df

def process_temperature_data(temp_values):
    sample = np.arange(len(temp_values))
    temp_data_celsius = (np.array(temp_values, dtype=float) / 132.48) + 25
    temp_data_celsius_avg = np.mean(temp_data_celsius) 
    temp_df = pd.DataFrame({"amostra": sample, "°C": temp_data_celsius_avg}) 
    return temp_df


# Processamento dos Dados 
df_acc_x, df_speed_x, df_gyro_x, df_degree_x = process_sensor_data(df['x_data'], df['x_gyro'])
df_acc_y, df_speed_y, df_gyro_y, df_degree_y = process_sensor_data(df['y_data'], df['y_gyro'])
df_acc_z, df_speed_z, df_gyro_z, df_degree_z = process_sensor_data(df['z_data'], df['z_gyro'])
df_temp = process_temperature_data(df['temp'])


# Gráfico 1: Aceleração e Velocidade em X
plt.figure(figsize=(12, 5)) 
plt.plot(df_acc_x[x_base_label], df_acc_x['m/s²'], label='Aceleração X (m/s²)')
plt.plot(df_speed_x[x_base_label], df_speed_x['m/s'], label='Velocidade X (m/s)')
plt.title('Aceleração e Velocidade em [X]')
plt.xlabel(x_base_label)
plt.ylabel('Valor')
plt.legend() 
plt.grid(True) 

# Gráfico 2: Aceleração e Velocidade em Y
plt.figure(figsize=(12, 5))
plt.plot(df_acc_y[x_base_label], df_acc_y['m/s²'], label='Aceleração Y (m/s²)')
plt.plot(df_speed_y[x_base_label], df_speed_y['m/s'], label='Velocidade Y (m/s)')
plt.title('Aceleração e Velocidade em [Y]')
plt.xlabel(x_base_label)
plt.ylabel('Valor')
plt.legend()
plt.grid(True)

# Gráfico 3: Aceleração e Velocidade em Z
plt.figure(figsize=(12, 5))
plt.plot(df_acc_z[x_base_label], df_acc_z['m/s²'], label='Aceleração Z (m/s²)')
plt.plot(df_speed_z[x_base_label], df_speed_z['m/s'], label='Velocidade Z (m/s)')
plt.title('Aceleração e Velocidade em [Z]')
plt.xlabel(x_base_label)
plt.ylabel('Valor')
plt.legend()
plt.grid(True)

# Gráfico 4: Velocidade Angular e Ângulo em X
plt.figure(figsize=(12, 5))
plt.plot(df_gyro_x[x_base_label], df_gyro_x['°/s'], label='Velocidade Angular X (°/s)')
plt.plot(df_degree_x[x_base_label], df_degree_x['°'], label='Ângulo X (°)')
plt.title('Velocidade Angular e Ângulo em [X]')
plt.xlabel(x_base_label)
plt.ylabel('Valor')
plt.legend()
plt.grid(True)

# Gráfico 5: Velocidade Angular e Ângulo em Y
plt.figure(figsize=(12, 5))
plt.plot(df_gyro_y[x_base_label], df_gyro_y['°/s'], label='Velocidade Angular Y (°/s)')
plt.plot(df_degree_y[x_base_label], df_degree_y['°'], label='Ângulo Y (°)')
plt.title('Velocidade Angular e Ângulo em [Y]')
plt.xlabel(x_base_label)
plt.ylabel('Valor')
plt.legend()
plt.grid(True)

# Gráfico 6: Velocidade Angular e Ângulo em Z
plt.figure(figsize=(12, 5))
plt.plot(df_gyro_z[x_base_label], df_gyro_z['°/s'], label='Velocidade Angular Z (°/s)')
plt.plot(df_degree_z[x_base_label], df_degree_z['°'], label='Ângulo Z (°)')
plt.title('Velocidade Angular e Ângulo em [Z]')
plt.xlabel(x_base_label)
plt.ylabel('Valor')
plt.legend()
plt.grid(True)

# Gráfico 7: Temperatura
plt.figure(figsize=(12, 5))
plt.plot(df_temp["amostra"], df_temp["°C"], label='Temperatura (°C)')
plt.title('Temperatura em [°C]')
plt.xlabel('Amostra') 
plt.ylabel('°C')
plt.legend()
plt.grid(True)
plt.show()

print("\nProcessamento e Geração de Gráficos Concluídos.")