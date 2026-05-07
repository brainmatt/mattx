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

from flask import Flask, jsonify, render_template
import re
import random

app = Flask(__name__)

# Initialize simulated cluster nodes
DEMO_NODES = {}

def initialize_demo_nodes():
    """Initialize 64 simulated cluster nodes with static IDs and IPs"""
    for node_id in range(1, 65):
        DEMO_NODES[node_id] = {
            "nodeid": str(node_id),
            "ipaddress": f"192.168.2.{node_id}",
            "cpuload": random.randint(2000, 3000),
            "memfree": random.randint(2000, 3000)
        }

def update_demo_nodes():
    """Update cpu_load and mem_free values with smooth random variations"""
    for node_id in DEMO_NODES:
        node = DEMO_NODES[node_id]
        
        # Update CPU Load with random variation
        variation_type = random.choice(['up', 'down', 'stay'])
        if variation_type == 'up':
            change = random.randint(10, 50)
            new_cpuload = int(node["cpuload"]) + change
        elif variation_type == 'down':
            change = random.randint(10, 50)
            new_cpuload = int(node["cpuload"]) - change
        else:  # stay the same
            new_cpuload = int(node["cpuload"])
        
        # Ensure CPU Load stays within limits [2000, 3000]
        new_cpuload = max(2000, min(3000, new_cpuload))
        node["cpuload"] = str(new_cpuload)
        
        # Update Memory Free with random variation
        variation_type = random.choice(['up', 'down', 'stay'])
        if variation_type == 'up':
            change = random.randint(10, 50)
            new_memfree = int(node["memfree"]) + change
        elif variation_type == 'down':
            change = random.randint(10, 50)
            new_memfree = int(node["memfree"]) - change
        else:  # stay the same
            new_memfree = int(node["memfree"])
        
        # Ensure Memory Free stays within limits [2000, 3000]
        new_memfree = max(2000, min(3000, new_memfree))
        node["memfree"] = str(new_memfree)

def parse_mattx_nodes():
    """Parse /proc/mattx/nodes and return structured data"""
    nodes = []
    
    try:
        with open('/proc/mattx/nodes', 'r') as f:
            lines = f.readlines()
        
        # Skip header lines until we find actual node data
        in_data_section = False
        for line in lines:
            line = line.strip()
            
            # Skip empty lines and header separators
            if not line or line.startswith('----') or line.startswith('MattX'):
                continue
            
            # Skip the "Balancer Enabled" line
            if line.startswith('Balancer'):
                continue
            
            # Skip the column header line
            if 'Node ID' in line:
                in_data_section = True
                continue
            
            # Process data lines
            if in_data_section and line:
                # Parse the node data using regex to handle multiple whitespace/tabs
                # Pattern: NodeID (optional Local), IP, CPULoad, MemFree
                match = re.match(r'(\d+)\s*(?:\(Local\))?\s+(\d+\.\d+\.\d+\.\d+)\s+(\d+)\s+(\d+)', line)
                
                if match:
                    node_id, ip_address, cpu_load, mem_free = match.groups()
                    nodes.append({
                        "nodeid": node_id,
                        "ipaddress": ip_address,
                        "cpuload": cpu_load,
                        "memfree": mem_free
                    })
    
    except FileNotFoundError:
        # Handle case where /proc/mattx/nodes doesn't exist
        return {"error": "File /proc/mattx/nodes not found"}
    except Exception as e:
        return {"error": str(e)}
    
    return nodes

@app.route('/mattx', methods=['GET'])
def mattx():
    """API endpoint to retrieve MattX cluster node information"""
    nodes_data = parse_mattx_nodes()
    
    if isinstance(nodes_data, dict) and "error" in nodes_data:
        return jsonify(nodes_data), 404
    
    return jsonify({"mattx": nodes_data})


@app.route('/demo', methods=['GET'])
def demo():
    """API endpoint to retrieve simulated 64 cluster nodes with smooth load variations"""
    # Update node metrics with random variations
    update_demo_nodes()
    
    # Convert nodes dict to list format
    nodes_list = list(DEMO_NODES.values())
    
    return jsonify({"mattx": nodes_list})


@app.route('/')
def webroot():
    return render_template('index.html')


@app.route('/democluster')
def democluster():
    return render_template('democluster.html')


if __name__ == '__main__':
    # Initialize demo nodes on startup
    initialize_demo_nodes()
    app.run(debug=True, host='0.0.0.0', port=5000)
