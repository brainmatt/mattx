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

import os
import json
from flask import Flask, render_template, jsonify

app = Flask(__name__)

def get_local_node_id():
    """Helper to find the local node ID from /proc/mattx/nodes"""
    try:
        with open('/proc/mattx/nodes', 'r') as f:
            for line in f:
                if "(Local)" in line:
                    parts = line.split()
                    return int(parts[0])
    except Exception:
        pass
    return 0

@app.route('/api/migrations')
def api_migrations():
    """Reads /proc/mattx/remote and returns active migrations as JSON"""
    migrations = []
    local_id = get_local_node_id()
    
    try:
        with open('/proc/mattx/remote', 'r') as f:
            for line in f:
                line = line.strip()
                if not line: continue
                # Format is "orig_pid:target_node"
                parts = line.split(':')
                if len(parts) == 2:
                    migrations.append({
                        "pid": int(parts[0]),
                        "home_node": local_id,
                        "target_node": int(parts[1])
                    })
    except Exception as e:
        print(f"Error reading /proc/mattx/remote: {e}")
        
    return jsonify(migrations)

@app.route('/api/nodes')
def api_nodes():
    """Reads /proc/mattx/nodes and returns the cluster map for the 3D Grid"""
    nodes = []
    try:
        with open('/proc/mattx/nodes', 'r') as f:
            lines = f.readlines()
            # Skip the 4 header lines
            for line in lines[4:]:
                if not line.strip() or "Balancer" in line or "MattXFS" in line or "Debug" in line or "Affinity" in line or "Migration" in line:
                    continue
                parts = line.split()
                if len(parts) >= 1:
                    node_id = int(parts[0])
                    is_local = "(Local)" in line
                    nodes.append({"id": node_id, "is_local": is_local})
    except Exception as e:
        print(f"Error reading /proc/mattx/nodes: {e}")
        
    return jsonify(nodes)

@app.route('/wormhole')
def wormhole_viewer():
    """Serves the 3D Process Migration Visualization"""
    return render_template('wormhole.html')


if __name__ == '__main__':
    app.run(debug=True, host='0.0.0.0', port=5001)

