#
# MattX - The Modern Single System Image (SSI) Cluster
# 
# Copyright (c) 2026 by Matthias Rechenburg
# All rights reserved.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
#
# Commercial licensing options are available upon request.
#

from flask import Flask, request, jsonify, render_template, redirect, url_for, session
import cv2
import base64

app = Flask(__name__)

move = ""

@app.route('/postmove', methods=['POST', 'GET'])
def handle_post():
    global move
    move = request.args.get('move')
    # Print the parameters to the terminal/console
    print(f"Received move: {move}")

    # Return a JSON response to the client
    return jsonify({
        "status": "success",
        "message": "Move submitted."
    }), 200



@app.route('/move', methods=['GET'])
def retrunmove():
    global move
    print("returning " + move)
    return move


@app.route('/', methods=['GET'])
def chess():
    return render_template('index.html')


if __name__ == '__main__':
    # Run the app in debug mode on port 5003
    app.run(debug=True, host='0.0.0.0', port=5003)


