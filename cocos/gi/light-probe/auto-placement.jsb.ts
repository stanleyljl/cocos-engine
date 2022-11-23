/*
 Copyright (c) 2021 Xiamen Yaji Software Co., Ltd.

 https://www.cocos.com/

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated engine source code (the "Software"), a limited,
  worldwide, royalty-free, non-assignable, revocable and non-exclusive license
 to use Cocos Creator solely to develop games on your target platforms. You shall
  not use Cocos Creator software for developing other software or tools that's
  used for developing games. You are not granted to publish, distribute,
  sublicense, and/or sell copies of Cocos Creator.

 The software or tools in this License Agreement are licensed, not sold.
 Xiamen Yaji Software Co., Ltd. reserves all rights not expressly granted to you.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
*/
import { Enum, _decorator } from "../../core";

const { ccclass, serializable } = _decorator;

/**
 * @zh
 * 自动放置类型
 * @en
 * The auto placement method
 */
 export const PlaceMethod = Enum({
  /**
   * @zh
   * 均匀放置
   * @en
   * place uniformly
   */
  UNIFORM: 0,

  /**
   * @zh
   * 自适应放置
   * @en
   * place adaptively
   */
  ADAPTIVE: 1,
});

export const PlacementInfo = jsb.PlacementInfo;
ccclass('cc.PlacementInfo')(PlacementInfo);

export const AutoPlacement = jsb.AutoPlacement;
ccclass('cc.AutoPlacement')(AutoPlacement);
