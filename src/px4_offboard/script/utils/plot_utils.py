import math
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.patches as patches
from collections import deque

from utils.navigation_utils import read_map_data

BLACK  = (0,0,0)
WHITE  = (255,255,255)
RED    = (0,0,255)
BLUE   = (255,0,0)
GREEN  = (0,255,0)
YELLOW = (0,255,255)

def plot_vehicle(handle, pos, heading, show_FOV=True, is_first=False):
    """
    plot the vehicle with FOV
    """
    FOV = 70 * np.pi / 180
    # location
    if is_first:
        origin, = handle.plot(pos[0], pos[1], marker='o', markersize=4, fillstyle='full', color='r')
    else:
        handle['origin'].set_xdata(pos[0])
        handle['origin'].set_ydata(pos[1])
    # heading
    L = 2
    forward_x = [pos[0], pos[0] + L*np.cos(heading)]
    forward_y = [pos[1], pos[1] + L*np.sin(heading)]
    right_x = [pos[0], pos[0] + L*np.cos(heading+np.pi/2)]
    right_y = [pos[1], pos[1] + L*np.sin(heading+np.pi/2)]
    if is_first:
        forward_line, = handle.plot(forward_x, forward_y, color='b', linewidth=2)
        right_line, = handle.plot(right_x, right_y, color='g', linewidth=2)
    else:
        handle['forward_line'].set_xdata(forward_x)
        handle['forward_line'].set_ydata(forward_y)
        handle['right_line'].set_xdata(right_x)
        handle['right_line'].set_ydata(right_y)

    if show_FOV:
        L = 5 / np.cos(FOV/2)
        left_point = [pos[0] + L*np.cos(heading+FOV/2), pos[1] + L*np.sin(heading+FOV/2)]
        right_point = [pos[0] + L*np.cos(heading-FOV/2), pos[1] + L*np.sin(heading-FOV/2)]
        path = [pos, left_point, right_point]
        if is_first:
            FOV_patch = handle.add_patch(patches.Polygon(path, color='w', linestyle='-', alpha=0.25))
        else:
            handle['FOV_patch'].set_xy(path)

    if is_first:
        return {'origin': origin,
                'forward_line': forward_line,
                'right_line': right_line, 
                'FOV_patch': FOV_patch}   


def plot_trajectory_history(handle, trajectory_history, is_first=False):
    """
    plot trajectory history
    """
    pos_x, pos_y = [], []

    for i in range(len(trajectory_history)):
        pos_x.append(trajectory_history[i][0])
        pos_y.append(trajectory_history[i][1])    

    if is_first:
        trajectory, = handle.plot(pos_x, pos_y, color='c', alpha=0.7, linewidth=2.0)
        return trajectory
    else:
        handle['trajectory_history'].set_xdata(pos_x)
        handle['trajectory_history'].set_ydata(pos_y)


class MapPlot():
    """
    plot the map of the environment
    """
    def __init__(self, map_path):
        self.fig, self.axes = plt.subplots()
        self.map_data = read_map_data(map_path)
        self.initialize_variables()
        self.initialize_map()

    def initialize_variables(self):
        self.has_initialized = False
        self.start_point = {}
        self.end_point = {}
        self.currnet_point = None
        self.pose_history = deque(maxlen=150)

    def initialize_map(self, disp_intervel=10):
        self.axes.plot(self.map_data['center'][:,0], self.map_data['center'][:,1], color='w', linewidth=0.5)
        self.axes.plot(self.map_data['upper'][:,0], self.map_data['upper'][:,1], color=(0.9290, 0.6940, 0.1250), linewidth=2.0)
        self.axes.plot(self.map_data['lower'][:,0], self.map_data['lower'][:,1], color=(0.9290, 0.6940, 0.1250), linewidth=2.0)
        self.axes.set_aspect('equal')
        self.axes.grid(alpha=0.15)
        x_range, y_range = self.axes.get_xlim(), self.axes.get_ylim()
        x_range = (disp_intervel*math.floor(x_range[0]/disp_intervel), disp_intervel*math.ceil(x_range[1]/disp_intervel))
        y_range = (disp_intervel*math.floor(y_range[0]/disp_intervel), disp_intervel*math.ceil(y_range[1]/disp_intervel))
        self.axes.set_xticks(range(x_range[0], x_range[1], disp_intervel))
        self.axes.set_xticklabels([])
        self.axes.set_yticks(range(y_range[0], y_range[1], disp_intervel))
        self.axes.set_yticklabels([])
        self.axes.tick_params(direction='out', length=0, color='w')
        self.axes.set_xlabel('x', fontsize=16)
        self.axes.set_ylabel('y', fontsize=16)
        self.fig.tight_layout()
        self.fig.canvas.mpl_connect('button_press_event', self.one_click) 

    def update_start_point(self, start_point):
        start_upper_idx = np.abs(self.map_data['upper'][:,0] - start_point[0]).argmin()
        start_lower_idx = np.abs(self.map_data['lower'][:,0] - start_point[0]).argmin()

        if not self.start_point: # empty
            self.start_point['value'] = start_point
            # self.start_point['line'], = self.axes.plot([self.map_data['upper'][start_upper_idx,0], self.map_data['lower'][start_lower_idx,0]], 
            #                             [self.map_data['upper'][start_upper_idx,1], self.map_data['lower'][start_lower_idx,1]], 
            #                             color='w', linestyle='-.', linewidth=1)
            self.start_point['dot'], = self.axes.plot(start_point[0], start_point[1], marker='o', markersize=10, fillstyle='none', color='g')
            self.start_point['patch'] = self.axes.add_patch(plt.Circle(start_point, 2, color='g', alpha=0.5))
        else:
            self.start_point['value'] = start_point
            # self.start_point['line'].set_xdata([self.map_data['upper'][start_upper_idx,0], self.map_data['lower'][start_lower_idx,0]])
            # self.start_point['line'].set_ydata([self.map_data['upper'][start_upper_idx,1], self.map_data['lower'][start_lower_idx,1]])
            self.start_point['dot'].set_xdata(start_point[0])
            self.start_point['dot'].set_ydata(start_point[1])
            self.start_point['patch'].set_center(start_point)

    def update_end_point(self, end_point):
        end_upper_idx = np.abs(self.map_data['upper'][:,0] - end_point[0]).argmin()
        end_lower_idx = np.abs(self.map_data['lower'][:,0] - end_point[0]).argmin()

        if not self.end_point: # empty
            self.end_point['value'] = end_point
            self.end_point['line'], = self.axes.plot([self.map_data['upper'][end_upper_idx,0], self.map_data['lower'][end_lower_idx,0]], 
                                        [self.map_data['upper'][end_upper_idx,1], self.map_data['lower'][end_lower_idx,1]], 
                                        color='w', linestyle='-.', linewidth=1)
            self.end_point['dot'], = self.axes.plot(end_point[0], end_point[1], marker='o', markersize=10, fillstyle='none', color='r')
            self.end_point['patch'] = self.axes.add_patch(plt.Circle(end_point, 2, color='r', alpha=0.5))
        else:
            self.end_point['value'] = end_point
            self.end_point['line'].set_xdata([self.map_data['upper'][end_upper_idx,0], self.map_data['lower'][end_lower_idx,0]] )
            self.end_point['line'].set_ydata([self.map_data['upper'][end_upper_idx,1], self.map_data['lower'][end_lower_idx,1]])
            self.end_point['dot'].set_xdata(end_point[0])
            self.end_point['dot'].set_ydata(end_point[1])
            self.end_point['patch'].set_center(end_point)

    def update_graph(self, pos, heading):
        self.currnet_point = [pos[0], pos[1]]
        self.pose_history.append([pos[0], pos[1], heading])
        if self.has_initialized:
            plot_vehicle(self.axes_dict, pos, heading, is_first=False)
            plot_trajectory_history(self.axes_dict, self.pose_history)
        else:
            self.axes_dict = plot_vehicle(self.axes, pos, heading, is_first=True)
            self.axes_dict['trajectory_history'] = plot_trajectory_history(self.axes, self.pose_history, is_first=True)
            self.update_start_point(self.currnet_point)
            self.has_initialized = True

    def one_click(self, event):
        if event.button == 1: # left click
            end_point = [event.xdata, event.ydata]
            self.update_end_point(end_point)
            self.update_start_point(self.currnet_point)
            print("Select end point: ({:.2f}, {:.2f})".format(event.xdata, event.ydata))
        elif event.button == 3: # right click
            for key, value in self.end_point.items():
                if key != 'value':  
                    value.remove()

            self.end_point = {}
            print("Clear end point.")

    def get_direction(self):
        if not self.end_point or not self.start_point:
            return 1
        
        if self.end_point['value'][0] > self.start_point['value'][0]:
            return 1
        else:
            return -1

    def get_end_point(self):
        if len(self.end_point) == 0:
            return None
        else:
            return self.end_point['value'] 

    def reset(self):
        self.axes.clear()
        self.initialize_variables()