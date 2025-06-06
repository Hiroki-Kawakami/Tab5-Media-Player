# Tab5-Media-Player
Play video file on M5Stack Tab5

## 使い方

USBストレージ/SDカードに保存してある、MJPEG+MP3でエンコードしたAVIの動画ファイルを再生することができます。  
USBとSDカードは、起動時に接続されているものを認識します。両方接続されている場合はUSBが優先でマウントされます。  
SDカードスロットは速度が遅く、データの読み込みが間に合わなくなることが多いため、USBを使用することを推奨します。

### 動画の変換方法

[FFmpeg](https://ffmpeg.org)を使用します。Homebrew等のパッケージマネージャーからインストールするか、公式サイトからダウンロードして使用してください。

- 解像度が1280x720以上の16:9の動画ファイルの場合
  - ```
    ffmpeg -i input.mp4 \
        -vf "transpose=2,scale=720:1280,fps=30" \
        -c:v mjpeg -pix_fmt yuv420p -q:v 15 \
        -c:a mp3 -ac 2 \
        -f avi output.avi
    ```
  - 各オプションの説明
    - `-i input.mp4`
      - 変換元のファイルを指定してください
    - `-vf "transpose=2,scale=720:1280,fps=30"`
      - 映像を90度回転、720x1280のサイズにスケール、フレームレートを30fpsに変換します
      - 720x1280にあらかじめ回転した状態でエンコードすると、M5Stack Tab5側で再生する際の回転処理が不要になるため安定して再生できます。
    - `-c:v mjpeg -pix_fmt yuv420p -q:v 15`
      - mjpeg形式、色空間はyuv420、品質を15で映像を圧縮
      - `-c:v`と`-pix_fmt`の値は固定で、これ以外の動画ファイルは再生できません。
      - `-q:v`の値は 2~31 で、値が小さいほど高画質でファイルサイズが大きくなります。
    - `-c:a mp3 -ac 2`
      - 2chステレオのmp3で音声を圧縮します
    - `-f avi output.avi`
      - 変換先のファイル形式はavi固定、ファイル名は任意の名前に変えてください
- 解像度が1280x720以下、フレームレートが30fps以下の動画ファイルの場合
  - ```
    ffmpeg -i input.mp4 \
        -c:v mjpeg -pix_fmt yuv420p -q:v 15 \
        -c:a mp3 -ac 2 \
        -f avi output.avi
    ```
  - 解像度が小さい場合はM5Stack Tab5側で自動で拡大/回転処理を行うため、`-vf`オプションは不要です
  
### 動画がうまく再生できない場合

- 音がぷつぷつ途切れて、ゆっくり再生される
  - データの読み出し速度が間に合っていません。SDカードスロットを使用している場合はカードリーダー等を使用してUSB経由で接続するか、動画変換時に以下オプションを変更してファイルサイズを減らしてください
    - `scale=720:1280` : 値を小さくして動画の解像度を下げる
    - `fps=30` : 値を小さくしてフレームレートを下げる。フレームレートが29.97等の整数値でない場合は30等の整数値にする
    - `-q:v 15` : 値を大きくして品質を下げる
- フレームドロップ
  - 60fps等の動画を再生しようとしてMJPEGのデコードと描画が間に合わない場合は、JTAG/UARTに `E (11528) AVIPlayer: Frame dropped.` が出力されてフレームドロップが発生します。
  - 描画できない分のデータは無駄なので、720pであれば45fps程度を上限に変換することをお勧めします。

### 動画変換の調整

再生するコンテンツやストレージの速度によって最適なオプションは変わります。以下をすることで、より高画質/高フレームレートを出すことができるかも。

- 読み出しが間に合いそうなレベルまで `-q:v` の値を上げる
- `-b:v`オプションで出力レートを平滑化する
- mozjpeg等のいい画質でJPEG圧縮できるエンコーダを使う
