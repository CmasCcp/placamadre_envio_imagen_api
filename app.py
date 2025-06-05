from dotenv import load_dotenv

from flask import Flask, jsonify, request, send_from_directory
from flask_cors import CORS
import csv,io, os

load_dotenv()
app = Flask(__name__)
CORS(app)

# Definir el directorio donde se guardarán las imágenes
UPLOAD_FOLDER = 'uploads'
app.config['UPLOAD_FOLDER'] = UPLOAD_FOLDER

# Extensiones permitidas para la imagen
ALLOWED_EXTENSIONS = {'jpg', 'jpeg', 'png'}

# Función para verificar las extensiones de archivo permitidas
def allowed_file(filename):
    return '.' in filename and filename.rsplit('.', 1)[1].lower() in ALLOWED_EXTENSIONS

# Endpoint para recibir la imagen como bytes
@app.route('/agregarImagen', methods=['POST'])
def agregar_imagen():
    # Verificar si la solicitud contiene datos
    if not request.data:
        return jsonify({"error": "No image data received"}), 400

    # Crear un nombre único para el archivo (puedes personalizarlo como quieras)
    filename = "received_image.jpg"
    filepath = os.path.join(UPLOAD_FOLDER, filename)

    try:
        # Guardar los datos binarios recibidos en el servidor
        with open(filepath, 'wb') as img_file:
            img_file.write(request.data)  # Guardar los bytes de la imagen
        return jsonify({"message": f"Image successfully saved at {filepath}"}), 200
    except Exception as e:
        return jsonify({"error": f"Failed to save image: {str(e)}"}), 500



@app.route('/verImagenes', methods=['GET'])
def ver_imagenes():
    """
    Devuelve una lista de los nombres de las imágenes almacenadas en la carpeta 'uploads'.
    """
    try:
        # Obtener una lista de todos los archivos en la carpeta uploads
        imagenes = os.listdir(app.config['UPLOAD_FOLDER'])
        imagenes = [img for img in imagenes]  # Filtrar solo imágenes
        return jsonify({"imagenes": imagenes}), 200
    except Exception as e:
        return jsonify({"error": f"Error al obtener las imágenes: {e}"}), 500


@app.route('/verImagen/<filename>', methods=['GET'])
def ver_imagen(filename):
    """
    Sirve una imagen desde el servidor para que pueda ser vista en el navegador.
    """
    try:
        # Enviar el archivo solicitado desde la carpeta uploads
        return send_from_directory(app.config['UPLOAD_FOLDER'], filename)
    except FileNotFoundError:
        return jsonify({"error": "Imagen no encontrada"}), 404



def generar_csv(data):
    if not data:
        return ''

    output = io.StringIO()
    writer = csv.DictWriter(output, fieldnames=data[0].keys())
    writer.writeheader()
    for row in data:
        writer.writerow(row)
    return output.getvalue()

def build_csv(df_pivoted):
    output = io.BytesIO()
    df_pivoted.to_csv(output, index=False, encoding="utf-8-sig")
    output.seek(0)
    for line in output:
        yield line    
    output.close()


if __name__ == "__main__":
    app.run(host='0.0.0.0', port=8084)
