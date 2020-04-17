# Kaldi 运行全过程

之前入了 beer 的大坑，实验效果不佳。经已经离职的大师兄再三劝导，终于还是用  pytorch-kaldi 吧。而这套处理音频模型的框架，又是基于语音届著名的工具包 kaldi。在安装和跑模型的过程中，可以发现它的构建思路和 beer 如出一辙。根据这两个包的出厂方 JHU，很容易联想到它们可能是同一个或隔壁实验室的作品。 

最好的入门材料是 kaldi 官网的教程。一开始跟着各种网站教程学习跑代码，遇到问题很难找到解答。而官网的设计者显然在各方面都为使用者考虑到了，教程写得非常详尽，并且提前规避了错误操作。如 [Data Preparation](http://kaldi-asr.org/doc/tutorial_running.html#tutorial_running_data_prep) 就声明了没有安装任务调度软件的话，就使用 `run.pl` 进行替代。在其他教程中未说明这点,将导致脚本大范围的修改。

## 数据准备

首先, 下载对应数据集到服务器. 有些脚本包含了下载脚本, 可以先尝试 `run.sh`. 也可以根据 `README.txt` 里面的提示去对应网址下载数据集.

Kaldi 并不直接使用这些数据, 而是经过 prepare.sh 将数据进行处理后放于 data 的子目录下. 具体数据处理的细节可参考 [Data preparation](http://kaldi-asr.org/doc/data_prep.html)
