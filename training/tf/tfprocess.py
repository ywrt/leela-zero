#!/usr/bin/env python3
#
#    This file is part of Leela Zero.
#    Copyright (C) 2017-2018 Gian-Carlo Pascutto
#
#    Leela Zero is free software: you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation, either version 3 of the License, or
#    (at your option) any later version.
#
#    Leela Zero is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU General Public License for more details.
#
#    You should have received a copy of the GNU General Public License
#    along with Leela Zero.  If not, see <http://www.gnu.org/licenses/>.

import numpy as np
import os
import random
import tensorflow as tf
import time

def weight_variable(shape):
    """Xavier initialization"""
    stddev = np.sqrt(2.0 / (sum(shape)))
    initial = tf.truncated_normal(shape, stddev=stddev)
    weights = tf.Variable(initial)
    tf.add_to_collection(tf.GraphKeys.WEIGHTS, weights)
    return weights

# Bias weights for layers not followed by BatchNorm
# We do not regularlize biases, so they are not
# added to the regularlizer collection
def bias_variable(shape):
    initial = tf.constant(0.0, shape=shape)
    return tf.Variable(initial)

# No point in learning bias weights as they are cancelled
# out by the BatchNorm layers's mean adjustment.
def bn_bias_variable(shape):
    initial = tf.constant(0.0, shape=shape)
    return tf.Variable(initial, trainable=False)

def conv2d(x, W):
    return tf.nn.conv2d(x, W, data_format='NCHW',
                        strides=[1, 1, 1, 1], padding='SAME')

# Class holding statistics
class Stats:
    def __init__(self):
        self.s = {}
    def add(self, stat_dict):
        for (k,v) in stat_dict.items():
            if k not in self.s:
                self.s[k] = []
            self.s[k].append(v)
    def n(self, name):
        return len(self.s[name] or [])
    def mean(self, name):
        return np.mean(self.s[name] or [0])
    def stddev_mean(self, name):
        # standard deviation in the sample mean.
        return math.sqrt(np.var(self.s[name] or [0]) / max(0.0001, (len(self.s[name]) - 1)))
    def str(self):
        return ', '.join([ "{}={:g}".format(k, np.mean(v or [0])) for k,v in self.s.items()])
    def clear(self):
        self.s = {}
    def summaries(self, tags):
        return [ tf.Summary.Value(tag=k, simple_value=self.mean(v)) for k,v in tags.items() ]

# Simple timer
class Timer:
    def __init__(self):
        self.last = time.time()
    def elapsed(self):
        # Return time since last call to 'elapsed()'
        t = time.time()
        e = t - self.last
        self.last = t
        return e

class TFProcess:
    def __init__(self):
        # Network structure
        self.RESIDUAL_FILTERS = 128
        self.RESIDUAL_BLOCKS = 6

        # For exporting
        self.weights = []

        gpu_options = tf.GPUOptions(per_process_gpu_memory_fraction=0.75)
        config = tf.ConfigProto(gpu_options=gpu_options)
        self.session = tf.Session(config=config)

        self.training = tf.placeholder(tf.bool)
        self.global_step = tf.Variable(0, name='global_step', trainable=False)

    def init(self, batch_size, macrobatch=1, logbase='leelalogs'):
        self.batch_size = batch_size
        self.macrobatch = macrobatch
        self.logbase = logbase
        # Input batch placeholders
        self.planes = tf.placeholder(tf.string, name='in_planes')
        self.probs = tf.placeholder(tf.string, name='in_probs')
        self.winner = tf.placeholder(tf.string, name='in_winner')

        # Mini-batches come as raw packed strings. Decode
        # into tensors to feed into network.
        planes = tf.decode_raw(self.planes, tf.uint8)
        probs = tf.decode_raw(self.probs, tf.float32)
        winner = tf.decode_raw(self.winner, tf.float32)

        planes = tf.to_float(planes)

        planes = tf.reshape(planes, (batch_size, 18, 19*19))
        probs = tf.reshape(probs, (batch_size, 19*19 + 1))
        winner = tf.reshape(winner, (batch_size, 1))

        self.init_net(planes, probs, winner)

    def init_net(self, planes, probs, winner):
        self.x = planes  # (tf.float32, [None, 18, 19 * 19])
        self.y_ = probs  # (tf.float32, [None, 362])
        self.z_ = winner # (tf.float32, [None, 1])
        self.batch_norm_count = 0
        self.y_conv, self.z_conv = self.construct_net(self.x)

        # Calculate loss on policy head
        cross_entropy = \
            tf.nn.softmax_cross_entropy_with_logits(labels=self.y_,
                                                    logits=self.y_conv)
        self.policy_loss = tf.reduce_mean(cross_entropy)

        # Loss on value head
        self.mse_loss = \
            tf.reduce_mean(tf.squared_difference(self.z_, self.z_conv))

        # Regularizer
        regularizer = tf.contrib.layers.l2_regularizer(scale=0.0001)
        reg_variables = tf.get_collection(tf.GraphKeys.WEIGHTS)
        self.reg_term = \
            tf.contrib.layers.apply_regularization(regularizer, reg_variables)

        # For training from a (smaller) dataset of strong players, you will
        # want to reduce the factor in front of self.mse_loss here.
        loss = 1.0 * self.policy_loss + 1.0 * self.mse_loss + self.reg_term

        # You need to change the learning rate here if you are training
        # from a self-play training set, for example start with 0.005 instead.
        opt = tf.train.MomentumOptimizer(
            learning_rate=0.05, momentum=0.9, use_nesterov=True)

        # Compute and accumulate gradients
        self.update_ops = tf.get_collection(tf.GraphKeys.UPDATE_OPS)
        total_grad=[]
        grad_ops=[]
        clear_var=[]
        with tf.control_dependencies(self.update_ops):
            self.grad_op_real = opt.compute_gradients(loss)
            for (g, v) in self.grad_op_real:
                if g is None:
                    total_grad.append((g,v))
                name = v.name.split(':')[0]
                gsum = tf.get_variable(name='gsum/'+name,
                                       shape=g.shape,
                                       trainable=False,
                                       initializer=tf.zeros_initializer)
                total_grad.append((gsum, v))
                grad_ops.append(tf.assign_add(gsum, g))
                clear_var.append(gsum)
        # Op to compute gradients and add to running total in 'gsum/'
        self.grad_op = tf.group(*grad_ops)

        # Op to apply accmulated gradients
        self.train_op = opt.apply_gradients(total_grad)

        zero_ops = []
        for g in clear_var:
            zero_ops.append(tf.assign(g, tf.zeros(shape=g.shape, dtype=g.dtype)))
        # Op to clear accumulated gradients
        self.clear_op = tf.group(*zero_ops)

        # Op to increment global step counter
        self.step_op = tf.assign_add(self.global_step, 1)

        correct_prediction = \
            tf.equal(tf.argmax(self.y_conv, 1), tf.argmax(self.y_, 1))
        correct_prediction = tf.cast(correct_prediction, tf.float32)
        self.accuracy = tf.reduce_mean(correct_prediction)

        # Summary part
        self.test_writer = tf.summary.FileWriter(
            os.path.join(os.getcwd(), self.logbase + "/test"), self.session.graph)
        self.train_writer = tf.summary.FileWriter(
            os.path.join(os.getcwd(), self.logbase + "/train"), self.session.graph)

        # Build checkpoint saver
        self.saver = tf.train.Saver()

        # Initialize all variables
        self.session.run(tf.global_variables_initializer())

    def replace_weights(self, new_weights):
        for e, weights in enumerate(self.weights):
            # Keyed batchnorm weights
            if isinstance(weights, str):
                work_weights = tf.get_default_graph().get_tensor_by_name(weights)
                new_weight = tf.constant(new_weights[e])
                self.session.run(tf.assign(work_weights, new_weight))
            elif weights.shape.ndims == 4:
                # Convolution weights need a transpose
                #
                # TF (kYXInputOutput)
                # [filter_height, filter_width, in_channels, out_channels]
                #
                # Leela/cuDNN/Caffe (kOutputInputYX)
                # [output, input, filter_size, filter_size]
                s = weights.shape.as_list()
                shape = [s[i] for i in [3, 2, 0, 1]]
                new_weight = tf.constant(new_weights[e], shape=shape)
                self.session.run(weights.assign(tf.transpose(new_weight, [2, 3, 1, 0])))
            elif weights.shape.ndims == 2:
                # Fully connected layers are [in, out] in TF
                #
                # [out, in] in Leela
                #
                s = weights.shape.as_list()
                shape = [s[i] for i in [1, 0]]
                new_weight = tf.constant(new_weights[e], shape=shape)
                self.session.run(weights.assign(tf.transpose(new_weight, [1, 0])))
            else:
                # Biases, batchnorm etc
                new_weight = tf.constant(new_weights[e], shape=weights.shape)
                self.session.run(weights.assign(new_weight))
        #This should result in identical file to the starting one
        #self.save_leelaz_weights('restored.txt')

    def restore(self, file):
        print("Restoring from {0}".format(file))
        self.saver.restore(self.session, file)

    def measure_loss(self, batch, training=False):
        # Measure loss over one batch. If training is true, also
        # accumulate the gradient and increment the global step.
        ops = [self.policy_loss, self.mse_loss, self.reg_term, self.accuracy ]
        if training:
            ops += [self.grad_op, self.step_op],
        r = self.session.run(ops, feed_dict={self.training: training,
                           self.planes: batch[0], self.probs: batch[1], self.winner: batch[2]})
        # Google's paper scales mse by 1/4 to a [0,1] range, so we do the same here
        return {'policy': r[0], 'mse': r[1]/4., 'reg': r[2],
                'accuracy': r[3], 'total': r[0]+r[1]+r[2] }

    def process(self, train_data, test_data):
        info_steps=1000
        stats = Stats()
        timer = Timer()
        while True:
            batch = next(train_data)
            # Measure losses and compute gradients for this batch.
            losses = self.measure_loss(batch, training=True)
            stats.add(losses)
            # fetch the current global step.
            steps = tf.train.global_step(self.session, self.global_step)
            if steps % self.macrobatch == (self.macrobatch-1):
                # Apply the accumulated gradients to the weights.
                self.session.run([self.train_op])
                # Clear the accumulated gradient.
                self.session.run([self.clear_op])

            if steps % info_steps == 0:
                speed = info_steps * self.batch_size / timer.elapsed()
                print("step {}, policy={:g} mse={:g} reg={:g} total={:g} ({:g} pos/s)".format(
                    steps, stats.mean('policy'), stats.mean('mse'), stats.mean('reg'),
                    stats.mean('total'), speed))
                summaries = stats.summaries({'Policy Loss': 'policy', 'MSE Loss': 'mse'})
                self.train_writer.add_summary(tf.Summary(value=summaries), steps)
                stats.clear()

            if steps % 8000 == 0:
                test_stats = Stats()
                test_batches = 800 # reduce sample mean variance by ~28x
                for _ in range(0, test_batches):
                    test_batch = next(test_data)
                    losses = self.measure_loss(test_batch, training=False)
                    test_stats.add(losses)
                summaries = test_stats.summaries({'Policy Loss': 'policy',
                                                  'MSE Loss': 'mse',
                                                  'Accuracy': 'accuracy'})
                self.test_writer.add_summary(tf.Summary(value=summaries), steps)
                print("step {}, policy={:g} training accuracy={:g}%, mse={:g}".\
                    format(steps, test_stats.mean('policy'),
                        test_stats.mean('accuracy')*100.0, test_stats.mean('mse')))

                # Write out current model and checkpoint
                path = os.path.join(os.getcwd(), "leelaz-model")
                save_path = self.saver.save(self.session, path, global_step=steps)
                print("Model saved in file: {}".format(save_path))
                leela_path = path + "-" + str(steps) + ".txt"
                self.save_leelaz_weights(leela_path)
                print("Leela weights saved to {}".format(leela_path))
                # Things have likely changed enough that stats are no longer valid.

    def save_leelaz_weights(self, filename):
        with open(filename, "w") as file:
            # Version tag
            file.write("1")
            for weights in self.weights:
                # Newline unless last line (single bias)
                file.write("\n")
                work_weights = None
                # Keyed batchnorm weights
                if isinstance(weights, str):
                    work_weights = tf.get_default_graph().get_tensor_by_name(weights)
                elif weights.shape.ndims == 4:
                    # Convolution weights need a transpose
                    #
                    # TF (kYXInputOutput)
                    # [filter_height, filter_width, in_channels, out_channels]
                    #
                    # Leela/cuDNN/Caffe (kOutputInputYX)
                    # [output, input, filter_size, filter_size]
                    work_weights = tf.transpose(weights, [3, 2, 0, 1])
                elif weights.shape.ndims == 2:
                    # Fully connected layers are [in, out] in TF
                    #
                    # [out, in] in Leela
                    #
                    work_weights = tf.transpose(weights, [1, 0])
                else:
                    # Biases, batchnorm etc
                    work_weights = weights
                nparray = work_weights.eval(session=self.session)
                wt_str = [str(wt) for wt in np.ravel(nparray)]
                file.write(" ".join(wt_str))

    def get_batchnorm_key(self):
        result = "bn" + str(self.batch_norm_count)
        self.batch_norm_count += 1
        return result

    def batch_norm(self, net):
        # The weights are internal to the batchnorm layer, so apply
        # a unique scope that we can store, and use to look them back up
        # later on.
        scope = self.get_batchnorm_key()
        self.weights.append(scope + "/batch_normalization/moving_mean:0")
        self.weights.append(scope + "/batch_normalization/moving_variance:0")
        with tf.variable_scope(scope):
            return tf.layers.batch_normalization(
                    net,
                    epsilon=1e-5, axis=1, fused=True,
                    center=True, scale=True,
                    training=self.training)


    def conv_block(self, inputs, filter_size, input_channels, output_channels):
        W_conv = weight_variable([filter_size, filter_size,
                                  input_channels, output_channels])
        b_conv = bn_bias_variable([output_channels])
        self.weights.append(W_conv)
        self.weights.append(b_conv)

        net = inputs
        net = conv2d(net, W_conv)
        net = self.batch_norm(net)
        net = tf.nn.relu(net)
        return net

    def residual_block(self, inputs, channels):
        # First convnet weights
        W_conv_1 = weight_variable([3, 3, channels, channels])
        b_conv_1 = bn_bias_variable([channels])
        self.weights.append(W_conv_1)
        self.weights.append(b_conv_1)

        # Second convnet weights
        W_conv_2 = weight_variable([3, 3, channels, channels])
        b_conv_2 = bn_bias_variable([channels])
        self.weights.append(W_conv_2)
        self.weights.append(b_conv_2)

        # Construct network
        net = inputs
        orig = tf.identity(net)
        net = conv2d(net, W_conv_1)
        net = self.batch_norm(net)
        net = tf.nn.relu(net)

        net = conv2d(net, W_conv_2)
        net = self.batch_norm(net)
        net = tf.add(net, orig)
        net = tf.nn.relu(net)

        return net

    def construct_net(self, planes):
        # NCHW format
        # batch, 18 channels, 19 x 19
        x_planes = tf.reshape(planes, [-1, 18, 19, 19])

        # Input convolution
        flow = self.conv_block(x_planes, filter_size=3,
                               input_channels=18,
                               output_channels=self.RESIDUAL_FILTERS)
        # Residual tower
        for _ in range(0, self.RESIDUAL_BLOCKS):
            flow = self.residual_block(flow, self.RESIDUAL_FILTERS)

        # Policy head
        conv_pol = self.conv_block(flow, filter_size=1,
                                   input_channels=self.RESIDUAL_FILTERS,
                                   output_channels=2)
        h_conv_pol_flat = tf.reshape(conv_pol, [-1, 2*19*19])
        W_fc1 = weight_variable([2 * 19 * 19, (19 * 19) + 1])
        b_fc1 = bias_variable([(19 * 19) + 1])
        self.weights.append(W_fc1)
        self.weights.append(b_fc1)
        h_fc1 = tf.add(tf.matmul(h_conv_pol_flat, W_fc1), b_fc1)

        # Value head
        conv_val = self.conv_block(flow, filter_size=1,
                                   input_channels=self.RESIDUAL_FILTERS,
                                   output_channels=1)
        h_conv_val_flat = tf.reshape(conv_val, [-1, 19*19])
        W_fc2 = weight_variable([19 * 19, 256])
        b_fc2 = bias_variable([256])
        self.weights.append(W_fc2)
        self.weights.append(b_fc2)
        h_fc2 = tf.nn.relu(tf.add(tf.matmul(h_conv_val_flat, W_fc2), b_fc2))
        W_fc3 = weight_variable([256, 1])
        b_fc3 = bias_variable([1])
        self.weights.append(W_fc3)
        self.weights.append(b_fc3)
        h_fc3 = tf.nn.tanh(tf.add(tf.matmul(h_fc2, W_fc3), b_fc3))

        return h_fc1, h_fc3
