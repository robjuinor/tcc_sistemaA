import socket  # Modulo para comandos websocket
from matplotlib import pyplot as plt  # Modulo para manipulação de graficos
import numpy as np  # Modulo para manipulacao matematica
import time  # Modulo para medir tempo
import scipy.io.wavfile  # Modulo para gravar audio como arquivo .wav
from credenciais_servidor import Host  # Importa indereco de IP do servidor
from credenciais_servidor import Port  # Importa porta socket do servidor
from tqdm import tqdm  # Modulo para barra de progresso 
import os # Modulo comandos de sistema para colocar diretorio de trabalho na pasta do arquivo

# Mudar diretorio de trabalho para pasta do arquivo:
dir_path = os.path.dirname(os.path.realpath(__file__))
os.chdir(dir_path)

# Conexao websocket (AF_INET = ipv4, SOCK_STREAM = TCP):
client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)  # Criando endereco websocket
client.connect((Host, Port))  # Conectando ao servidor do ESP32

# Variaveis:
buffsize =  16  # Tamanho do buffer de  leitura do I2S (deve ser igual ao do servidor)
freq_amost = 44100  # Frequencia de amostragem do periferico I2S (deve ser igual a do servidor)
namost = int(65536 * 2)  # Numero de amostras desejado (deve ser divisivel por 4)
razao_buffer_amost = 2  # Razao entre numero de elementos de um buffer e quantas amostras ele representa (um buffer de 8 bits precisa de dois elementos para  cada amostra de 16 bits) (deve ser igual a do servidor) 
ngravacoes = int(namost * razao_buffer_amost / buffsize)  # Numero de vezes que o cliente vai receber buffers
jsinal = []  # Inicializando vetor de buffers recebidos

# Enviando configuracoes ao servidor:
client.send(("A" + str(namost) + "X" ).encode("utf-8"))  # Enviando configuracoes (escalonavel para incluir varias instrucoes no formato: Aconf1Aconf2Aconf3...X, sendo conf1 igual a configuracao 1 e assim por diante)

# Comeca a receber buffers:
inicio = time.time()
for k in tqdm(range(ngravacoes)):
    jsinal.append(client.recv(buffsize))
fim = time.time()
print("Duracao: " + str(fim - inicio))

# Traduz os buffers em amostras (a cada dois elementos de 8 bits, uma amostra):
sinal = np.zeros(namost)  # Sinal de audio com zeros
for l in range(len(jsinal)):
    for c in range(0, buffsize, razao_buffer_amost):
        sinal[int(l*buffsize/razao_buffer_amost) + int(c/razao_buffer_amost)] = int.from_bytes(jsinal[l][c : c + razao_buffer_amost], byteorder='big', signed=True)

# Grafico do sinal:
plt.ion()
fig, ax = plt.subplots()
ax.plot(sinal)
fig.show()

# Escrevendo audio como arquivo wav
nome = "audio_gravado_inmp441"
scipy.io.wavfile.write(nome + ".wav", freq_amost, (sinal/np.max(np.abs(sinal))))