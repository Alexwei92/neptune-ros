import torch
import os
import cv2
import numpy as np
from torchvision import transforms
import tensorrt as trt

from models import EndToEnd
from utils import tensorrt_utils

########## GLOBAL VARIABLES ############
curr_dir        = os.path.dirname(os.path.abspath(__file__))
parent_dir      = os.path.dirname(curr_dir)
config_dir      = os.path.join(parent_dir, 'configs')

endToend_model_config = {
    'name': 'end_to_end',
    'in_channels': 3,
    'input_dim': 128,
    'extra_dim': 6,
    'enable_extra': False,
}

########################################

class EndToEndController():
    """
    End-To-End Controller
    """
    def __init__(self, **kwargs): 
        self.configure(**kwargs)
        self.load_model(**kwargs)
 
    def configure(self, **kwargs):
        """
        Configure
        """
        self.input_dim          = endToend_model_config['input_dim']
        self.device             = torch.device("cuda:0" if torch.cuda.is_available() else "cpu")
        self.transform_composed = transforms.Compose([ 
                                    transforms.ToTensor(),
                                    transforms.Normalize((0.5), (0.5)),
                                ])
        self.enable_extra       = kwargs['enable_extra']
        
    def load_model(self, model_weight_path, **kwargs):
        """
        Load Model
        """
        self.model = EndToEnd(**endToend_model_config).to(self.device)
        model_weight = torch.load(model_weight_path)
        self.model.load_state_dict(model_weight)

    def predict(self, image_color, is_bgr=True, state_extra=None):
        """
        Predict the action
        """
        image_np = image_color.copy() # hard copy
        if is_bgr:
            image_np = cv2.cvtColor(image_np, cv2.COLOR_BGR2RGB)
        image_np = cv2.resize(image_np, (self.model.input_dim, self.model.input_dim))
        image_tensor = self.transform_composed(image_np)
        
        self.model.eval()
        with torch.no_grad():
            if state_extra is not None:
                # if not enable state_extra
                if not self.enable_extra:
                    state_extra = np.zeros(endToend_model_config['extra_dim'])
                state_extra = state_extra.astype(np.float32)
                y_pred = self.model(image_tensor.unsqueeze(0).to(self.device), torch.from_numpy(state_extra).unsqueeze(0).to(self.device))
            else:
                y_pred = self.model(image_tensor.unsqueeze(0).to(self.device))
                
            y_pred = y_pred.cpu().item()
        
        return y_pred


class EndToEndController_TRT():
    """
    End-To-End Controller (run with TensorRT)
    """
    def __init__(self, **kwargs): 
        self.configure(**kwargs)
        self.load_engine(**kwargs)
 
    def configure(self, **kwargs):
        """
        Configure
        """
        self.input_dim          = endToend_model_config['input_dim']
        self.transform_composed = transforms.Compose([ 
                                    transforms.ToTensor(),
                                    transforms.Normalize((0.5), (0.5)),
                                ])
        self.enable_extra       = kwargs['enable_extra']

    def load_engine(self, tensorrt_engine_path, **kwargs):
        """
        Load TensorRT Engine
        """
        tensorrt_logger = trt.Logger(trt.Logger.ERROR)
        tensorrt_runtime = trt.Runtime(tensorrt_logger)
        with open(tensorrt_engine_path, 'rb') as f:
            serialized_engine  = f.read()
        self.engine = tensorrt_runtime.deserialize_cuda_engine(serialized_engine)

        self.inputs, self.outputs, self.bindings, self.stream = tensorrt_utils.allocate_buffers(self.engine)

    def predict(self, image_color, is_bgr=True, state_extra=None):
        """
        Predict the action
        """
        image_np = image_color.copy() # hard copy
        if is_bgr:
            image_np = cv2.cvtColor(image_np, cv2.COLOR_BGR2RGB)
        image_np = cv2.resize(image_np, (self.input_dim, self.input_dim))
        image_tensor = self.transform_composed(image_np)

        with self.engine.create_execution_context() as context:
            self.inputs[0].host = np.array(image_tensor, dtype=np.float32)
            # if not enable state_extra
            if not self.enable_extra:
                state_extra = np.zeros(endToend_model_config['extra_dim'])
            self.inputs[1].host = np.array(state_extra, dtype=np.float32)

            y_pred = tensorrt_utils.do_inference(
                context,
                self.bindings,
                self.inputs,
                self.outputs,
                self.stream,
            )[0][0]

        return y_pred